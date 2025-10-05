#include "audio_service.h"
#include <esp_log.h>
#include <cstring>

#if CONFIG_USE_AUDIO_PROCESSOR
#include "processors/afe_audio_processor.h"
#else
#include "processors/no_audio_processor.h"
#endif

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
#include "wake_words/afe_wake_word.h"
#include "wake_words/custom_wake_word.h"
#else
#include "wake_words/esp_wake_word.h"
#endif

#define TAG "AudioService"

// Sanity limit to guard against malformed packets causing huge allocations
static constexpr size_t MAX_OPUS_PAYLOAD_SIZE = 4096;  // typical Opus packet << 1275 bytes; 4KB is safe headroom


AudioService::AudioService() {
    event_group_ = xEventGroupCreate();
}

AudioService::~AudioService() {
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
}


void AudioService::Initialize(AudioCodec* codec) {
    codec_ = codec;
    codec_->Start();

    /* Setup the audio codec */
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(codec->output_sample_rate(), 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_ = std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_->SetComplexity(0);

    if (codec->input_sample_rate() != 16000) {
        input_resampler_.Configure(codec->input_sample_rate(), 16000);
        reference_resampler_.Configure(codec->input_sample_rate(), 16000);
    }

#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_ = std::make_unique<AfeAudioProcessor>();
#else
    audio_processor_ = std::make_unique<NoAudioProcessor>();
#endif

    audio_processor_->OnOutput([this](std::vector<int16_t>&& data) {
        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));
    });

    audio_processor_->OnVadStateChange([this](bool speaking) {
        voice_detected_ = speaking;
        if (callbacks_.on_vad_change) {
            callbacks_.on_vad_change(speaking);
        }
    });

    esp_timer_create_args_t audio_power_timer_args = {
        .callback = [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->CheckAndUpdateAudioPowerState();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_power_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&audio_power_timer_args, &audio_power_timer_);
}

void AudioService::Start() {
    service_stopped_ = false;
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    esp_timer_start_periodic(audio_power_timer_, 1000000);

#if CONFIG_USE_AUDIO_PROCESSOR
    /* Start the audio input task */
    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 3, this, 8, &audio_input_task_handle_, 0);

    /* Start the audio output task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
    }, "audio_output", 2048 * 2, this, 4, &audio_output_task_handle_);
#else
    /* Start the audio input task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 2, this, 8, &audio_input_task_handle_);

    /* Start the audio output task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
    }, "audio_output", 2048, this, 4, &audio_output_task_handle_);
#endif

    /* Start the opus codec task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->OpusCodecTask();
        vTaskDelete(NULL);
    }, "opus_codec", 2048 * 13, this, 2, &opus_codec_task_handle_);
    
    /* Start the dedicated opus decode task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->OpusDecodeTask();
        vTaskDelete(NULL);
    }, "opus_decode", 2048 * 8, this, 3, &opus_decode_task_handle_);  // 更高优先级
}



void AudioService::OpusDecodeTask() {
    ESP_LOGI(TAG, "Opus decode task started");
    
    // 预分配PCM缓冲区，避免频繁内存分配
    std::vector<int16_t> resampled_buffer;
    resampled_buffer.reserve(PCM_BUFFER_SIZE);
    
    int packets_processed = 0;
    
    while (!service_stopped_) {
        std::unique_ptr<AudioStreamPacket> packet = nullptr;
        
        // 安全地从队列中提取packet
        {
            std::unique_lock<std::mutex> lock(audio_queue_mutex_);
            
            // 等待队列中有数据或服务停止
            audio_queue_cv_.wait(lock, [this]() { 
                return !audio_decode_queue_.empty() || service_stopped_; 
            });
            
            if (service_stopped_) {
                ESP_LOGI(TAG, "Service stopped, exiting decode task. Processed %d packets total", packets_processed);
                break;
            }
            
            if (audio_decode_queue_.empty()) {
                ESP_LOGV(TAG, "Decode queue empty, continuing wait");
                continue;
            }
            
            // 如果解码队列过长，记录日志但继续处理
            if (audio_decode_queue_.size() > MAX_DECODE_TASKS_IN_QUEUE * 0.8) {
                // 降低日志级别，避免频繁告警污染输出
                ESP_LOGD(TAG, "Decode queue is getting full: %d packets", audio_decode_queue_.size());
            }
            
            if (audio_playback_queue_.size() >= MAX_PLAYBACK_TASKS_IN_QUEUE) {
                // 等待播放队列有空间，确保不丢包
                ESP_LOGD(TAG, "Playback queue full (%d), waiting for space", audio_playback_queue_.size());
                audio_queue_cv_.wait(lock, [this]() {
                    return audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE || service_stopped_;
                });
                
                if (service_stopped_) {
                    ESP_LOGI(TAG, "Service stopped while waiting for playback queue space");
                    break;
                }
            }

            // 安全地移动packet到本地变量
            packet = std::move(audio_decode_queue_.front());
            audio_decode_queue_.pop_front();
            audio_queue_cv_.notify_all();
        } // 释放锁
        
        // 验证packet有效性（在锁外进行）
        if (!packet) {
            ESP_LOGE(TAG, "Invalid packet received, skipping");
            continue;
        }
        
        if (packet->payload.empty()) {
            ESP_LOGW(TAG, "Empty payload received, skipping");
            continue;
        }

        // 提取所需数据
        int sample_rate = packet->sample_rate;
        int frame_duration = packet->frame_duration;
        uint32_t timestamp = packet->timestamp;
        
        // 载荷健壮性校验，避免异常巨大包触发 length_error/内存碎片化
        const size_t payload_size = packet->payload.size();
        if (payload_size == 0 || payload_size > MAX_OPUS_PAYLOAD_SIZE) {
            ESP_LOGE(TAG, "Invalid opus payload size: %u, dropping (sr=%d, ts=%u)", (unsigned)payload_size, sample_rate, timestamp);
            continue;
        }

        // 创建payload的拷贝而不是移动，避免移动后的析构问题
        std::vector<uint8_t> payload_copy;
        payload_copy.resize(payload_size);
        memcpy(payload_copy.data(), packet->payload.data(), payload_size);
        
        ESP_LOGV(TAG, "Processing packet: payload_size=%d, sample_rate=%d, timestamp=%u", 
                 payload_copy.size(), sample_rate, timestamp);
        
        // 不再手动清理payload，直接让packet在作用域结束时自然析构
        // 这样避免了手动调用clear()和shrink_to_fit()可能导致的内存问题

        auto task = std::make_unique<AudioTask>();
        if (!task) {
            ESP_LOGE(TAG, "Failed to allocate AudioTask, memory issue");
            continue;
        }
        
        task->type = kAudioTaskTypeDecodeToPlaybackQueue;
        task->timestamp = timestamp;

        SetDecodeSampleRate(sample_rate, frame_duration);
        
        ESP_LOGD(TAG, "Decoding packet: size=%d, sample_rate=%d, timestamp=%u", 
                 payload_copy.size(), sample_rate, timestamp);
        
        if (opus_decoder_->Decode(std::move(payload_copy), task->pcm)) {
            ESP_LOGV(TAG, "Successfully decoded packet to %d PCM samples", task->pcm.size());
            
            if (opus_decoder_->sample_rate() != codec_->output_sample_rate()) {
                int target_size = output_resampler_.GetOutputSamples(task->pcm.size());
                std::vector<int16_t> resampled(target_size);
                output_resampler_.Process(task->pcm.data(), task->pcm.size(), resampled.data());
                task->pcm = std::move(resampled);
                ESP_LOGV(TAG, "Resampled to %d samples", task->pcm.size());
            }

            {
                std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                audio_playback_queue_.push_back(std::move(task));
                audio_queue_cv_.notify_all();
                packets_processed++;
                
                if (packets_processed % 100 == 0) {
                    ESP_LOGI(TAG, "Processed %d packets, playback queue size: %d", 
                             packets_processed, audio_playback_queue_.size());
                }
            }
        } else {
            ESP_LOGE(TAG, "Failed to decode audio packet (timestamp=%u, size=%d)", 
                     timestamp, payload_copy.size());
        }
    }

    ESP_LOGW(TAG, "Opus decode task stopped. Total packets processed: %d", packets_processed);
}

void AudioService::Stop() {
    esp_timer_stop(audio_power_timer_);
    service_stopped_ = true;
    xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
        AS_EVENT_WAKE_WORD_RUNNING |
        AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    // 通知所有等待的任务
    audio_queue_cv_.notify_all();
    
    // 等待一小段时间让任务有机会检查service_stopped_标志
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // 现在安全地清理队列
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        ESP_LOGI(TAG, "Clearing queues: encode=%d, decode=%d, playback=%d, testing=%d", 
                 audio_encode_queue_.size(), audio_decode_queue_.size(), 
                 audio_playback_queue_.size(), audio_testing_queue_.size());
        
        audio_encode_queue_.clear();
        audio_decode_queue_.clear();
        audio_playback_queue_.clear();
        audio_testing_queue_.clear();
        audio_queue_cv_.notify_all();
    }
    
    ESP_LOGI(TAG, "AudioService stopped and queues cleared");
}

bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples) {
    if (!codec_->input_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableInput(true);
    }

    if (codec_->input_sample_rate() != sample_rate) {
        data.resize(samples * codec_->input_sample_rate() / sample_rate * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
        if (codec_->input_channels() == 2) {
            auto mic_channel = std::vector<int16_t>(data.size() / 2);
            auto reference_channel = std::vector<int16_t>(data.size() / 2);
            for (size_t i = 0, j = 0; i < mic_channel.size(); ++i, j += 2) {
                mic_channel[i] = data[j];
                reference_channel[i] = data[j + 1];
            }
            auto resampled_mic = std::vector<int16_t>(input_resampler_.GetOutputSamples(mic_channel.size()));
            auto resampled_reference = std::vector<int16_t>(reference_resampler_.GetOutputSamples(reference_channel.size()));
            input_resampler_.Process(mic_channel.data(), mic_channel.size(), resampled_mic.data());
            reference_resampler_.Process(reference_channel.data(), reference_channel.size(), resampled_reference.data());
            data.resize(resampled_mic.size() + resampled_reference.size());
            for (size_t i = 0, j = 0; i < resampled_mic.size(); ++i, j += 2) {
                data[j] = resampled_mic[i];
                data[j + 1] = resampled_reference[i];
            }
        } else {
            auto resampled = std::vector<int16_t>(input_resampler_.GetOutputSamples(data.size()));
            input_resampler_.Process(data.data(), data.size(), resampled.data());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
    }

    /* Update the last input time */
    last_input_time_ = std::chrono::steady_clock::now();
    debug_statistics_.input_count++;

#if CONFIG_USE_AUDIO_DEBUGGER
    // 音频调试：发送原始音频数据
    if (audio_debugger_ == nullptr) {
        audio_debugger_ = std::make_unique<AudioDebugger>();
    }
    audio_debugger_->Feed(data);
#endif

    return true;
}

void AudioService::AudioInputTask() {
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
            AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING,
            pdFALSE, pdFALSE, portMAX_DELAY);

        if (service_stopped_) {
            break;
        }
        if (audio_input_need_warmup_) {
            audio_input_need_warmup_ = false;
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        /* Used for audio testing in NetworkConfiguring mode by clicking the BOOT button */
        if (bits & AS_EVENT_AUDIO_TESTING_RUNNING) {
            if (audio_testing_queue_.size() >= AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS) {
                ESP_LOGW(TAG, "Audio testing queue is full, stopping audio testing");
                EnableAudioTesting(false);
                continue;
            }
            std::vector<int16_t> data;
            int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
            if (ReadAudioData(data, 16000, samples)) {
                // If input channels is 2, we need to fetch the left channel data
                if (codec_->input_channels() == 2) {
                    auto mono_data = std::vector<int16_t>(data.size() / 2);
                    for (size_t i = 0, j = 0; i < mono_data.size(); ++i, j += 2) {
                        mono_data[i] = data[j];
                    }
                    data = std::move(mono_data);
                }
                PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue, std::move(data));
                continue;
            }
        }

        /* Feed the wake word */
        if (bits & AS_EVENT_WAKE_WORD_RUNNING) {
            std::vector<int16_t> data;
            int samples = wake_word_->GetFeedSize();
            if (samples > 0) {
                if (ReadAudioData(data, 16000, samples)) {
                    wake_word_->Feed(data);
                    continue;
                }
            }
        }

        /* Feed the audio processor */
        if (bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING) {
            std::vector<int16_t> data;
            int samples = audio_processor_->GetFeedSize();
            if (samples > 0) {
                if (ReadAudioData(data, 16000, samples)) {
                    audio_processor_->Feed(std::move(data));
                    continue;
                }
            }
        }

        ESP_LOGE(TAG, "Should not be here, bits: %lx", bits);
        break;
    }

    ESP_LOGW(TAG, "Audio input task stopped");
}

void AudioService::AudioOutputTask() {
    ESP_LOGI(TAG, "Audio output task started");
    
    while (!service_stopped_) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() { return !audio_playback_queue_.empty() || service_stopped_; });
        
        if (service_stopped_) {
            break;
        }
        
        // 只在队列大小变化较大时打印日志
        static size_t last_queue_size = 0;
        if (audio_playback_queue_.size() != last_queue_size && 
            (audio_playback_queue_.size() == 0 || audio_playback_queue_.size() > 5 || 
             abs((int)audio_playback_queue_.size() - (int)last_queue_size) > 3)) {
            ESP_LOGI(TAG, "Audio playback queue size: %d", audio_playback_queue_.size());
            last_queue_size = audio_playback_queue_.size();
        }
        
        auto task = std::move(audio_playback_queue_.front());
        audio_playback_queue_.pop_front();
        audio_queue_cv_.notify_all();
        lock.unlock();

        if (!codec_->output_enabled()) {
            ESP_LOGI(TAG, "Enabling audio output (codec was disabled)");
            esp_timer_stop(audio_power_timer_);
            esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
            codec_->EnableOutput(true);
        }

        // 移除每次播放都打印的日志
        codec_->OutputData(task->pcm);
        last_output_time_ = std::chrono::steady_clock::now();
        debug_statistics_.playback_count++;
    }

    ESP_LOGW(TAG, "Audio output task stopped");
}

void AudioService::OpusCodecTask() {
    ESP_LOGI(TAG, "Opus codec task started (encoding only)");
    
    // 性能监控变量
    static uint32_t last_performance_log_time = 0;
    static uint32_t encode_count_since_last_log = 0;
    
    while (!service_stopped_) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() { 
            return !audio_encode_queue_.empty() || service_stopped_; 
        });
        
        if (service_stopped_) {
            break;
        }
        
        // 每5秒打印一次性能统计
        uint32_t current_time = esp_log_timestamp();
        if (current_time - last_performance_log_time > 5000) {
            ESP_LOGD(TAG, "Encoding performance: %d packets in last 5s. Queue sizes: encode=%d, send=%d",
                     encode_count_since_last_log,
                     audio_encode_queue_.size(), audio_send_queue_.size());
            last_performance_log_time = current_time;
            encode_count_since_last_log = 0;
        }
        
        /* Encode the audio to send queue */
        if (!audio_encode_queue_.empty() && audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE) {
            auto task = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->frame_duration = OPUS_FRAME_DURATION_MS;
            packet->sample_rate = 16000;
            packet->timestamp = task->timestamp;
            if (!opus_encoder_->Encode(std::move(task->pcm), packet->payload)) {
                ESP_LOGE(TAG, "Failed to encode audio");
                continue;
            }

            if (task->type == kAudioTaskTypeEncodeToSendQueue) {
                {
                    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                    audio_send_queue_.push_back(std::move(packet));
                }
                if (callbacks_.on_send_queue_available) {
                    callbacks_.on_send_queue_available();
                }
            } else if (task->type == kAudioTaskTypeEncodeToTestingQueue) {
                std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                audio_testing_queue_.push_back(std::move(packet));
            }
            debug_statistics_.encode_count++;
            encode_count_since_last_log++;
            lock.lock();
        }
    }

    ESP_LOGW(TAG, "Opus codec task stopped");
}

void AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    if (opus_decoder_->sample_rate() == sample_rate && opus_decoder_->duration_ms() == frame_duration) {
        return;
    }

    opus_decoder_.reset();
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(sample_rate, 1, frame_duration);

    auto codec = Board::GetInstance().GetAudioCodec();
    // Guard against using decoder getters if initialization failed; use requested sample_rate instead
    if (sample_rate != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", sample_rate, codec->output_sample_rate());
        output_resampler_.Configure(sample_rate, codec->output_sample_rate());
    }
}

void AudioService::PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm) {
    auto task = std::make_unique<AudioTask>();
    task->type = type;
    task->pcm = std::move(pcm);
    
    /* Push the task to the encode queue */
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);

    /* If the task is to send queue, we need to set the timestamp */
    if (type == kAudioTaskTypeEncodeToSendQueue && !timestamp_queue_.empty()) {
        if (timestamp_queue_.size() <= MAX_TIMESTAMPS_IN_QUEUE) {
            task->timestamp = timestamp_queue_.front();
        } else {
            ESP_LOGW(TAG, "Timestamp queue (%u) is full, dropping timestamp", timestamp_queue_.size());
        }
        timestamp_queue_.pop_front();
    }

    audio_queue_cv_.wait(lock, [this]() { return audio_encode_queue_.size() < MAX_ENCODE_TASKS_IN_QUEUE; });
    audio_encode_queue_.push_back(std::move(task));
    audio_queue_cv_.notify_all();
}

bool AudioService::PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait) {
    if (service_stopped_) {
        ESP_LOGW(TAG, "Service stopped, dropping packet");
        return false;
    }
    
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    
    if (audio_decode_queue_.size() >= MAX_DECODE_TASKS_IN_QUEUE) {
        // 将队列满的告警降为调试级别，减少运行时噪音
        ESP_LOGD(TAG, "Decode queue is full (%d packets), wait=%s", audio_decode_queue_.size(), wait ? "true" : "false");
        if (wait) {
            // 等待队列有空间
            audio_queue_cv_.wait(lock, [this]() { return audio_decode_queue_.size() < MAX_DECODE_TASKS_IN_QUEUE || service_stopped_; });
            if (service_stopped_) {
                ESP_LOGW(TAG, "Service stopped while waiting, dropping packet");
                return false;
            }
        } else {
            // 队列满时，暂时阻塞而不是丢包，给解码任务更多时间处理
            ESP_LOGD(TAG, "Decode queue full, waiting briefly for space...");
            auto timeout = std::chrono::milliseconds(10); // 短暂等待10ms
            if (audio_queue_cv_.wait_for(lock, timeout, [this]() { return audio_decode_queue_.size() < MAX_DECODE_TASKS_IN_QUEUE || service_stopped_; })) {
                if (service_stopped_) {
                    ESP_LOGW(TAG, "Service stopped during timeout, dropping packet");
                    return false;
                }
                ESP_LOGD(TAG, "Queue space available after brief wait");
            } else {
                ESP_LOGD(TAG, "Queue still full after timeout, packet may be delayed");
                // 即使超时也要添加包，让系统自然处理
            }
        }
    }
    
    // 再次检查服务状态，避免在停止过程中添加新包
    if (service_stopped_) {
        ESP_LOGW(TAG, "Service stopped before adding packet, dropping");
        return false;
    }
    
    audio_decode_queue_.push_back(std::move(packet));
    audio_queue_cv_.notify_all();
    return true;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_send_queue_.empty()) {
        return nullptr;
    }
    auto packet = std::move(audio_send_queue_.front());
    audio_send_queue_.pop_front();
    audio_queue_cv_.notify_all();
    return packet;
}

void AudioService::EncodeWakeWord() {
    if (wake_word_) {
        wake_word_->EncodeWakeWordData();
    }
}

const std::string& AudioService::GetLastWakeWord() const {
    return wake_word_->GetLastDetectedWakeWord();
}

std::unique_ptr<AudioStreamPacket> AudioService::PopWakeWordPacket() {
    auto packet = std::make_unique<AudioStreamPacket>();
    if (wake_word_->GetWakeWordOpus(packet->payload)) {
        return packet;
    }
    return nullptr;
}

void AudioService::EnableWakeWordDetection(bool enable) {
    if (!wake_word_) {
        return;
    }

    ESP_LOGD(TAG, "%s wake word detection", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!wake_word_initialized_) {
            if (!wake_word_->Initialize(codec_, models_list_)) {
                ESP_LOGE(TAG, "Failed to initialize wake word");
                return;
            }
            wake_word_initialized_ = true;
        }
        wake_word_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    } else {
        wake_word_->Stop();
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    }
}

void AudioService::EnableVoiceProcessing(bool enable) {
    ESP_LOGI(TAG, "%s voice processing", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!audio_processor_initialized_) {
            audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_);
            audio_processor_initialized_ = true;
        }

        /* We should make sure no audio is playing */
        ResetDecoder();
        audio_input_need_warmup_ = true;
        ESP_LOGI(TAG, "Starting audio processor, feed_size=%d", audio_processor_->GetFeedSize());
        audio_processor_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    } else {
        ESP_LOGI(TAG, "Stopping audio processor");
        audio_processor_->Stop();
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    }
}

void AudioService::EnableAudioTesting(bool enable) {
    ESP_LOGI(TAG, "%s audio testing", enable ? "Enabling" : "Disabling");
    if (enable) {
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
        /* Copy audio_testing_queue_ to audio_decode_queue_ */
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_decode_queue_ = std::move(audio_testing_queue_);
        audio_queue_cv_.notify_all();
    }
}

void AudioService::EnableDeviceAec(bool enable) {
    ESP_LOGI(TAG, "%s device AEC", enable ? "Enabling" : "Disabling");
    if (!audio_processor_initialized_) {
        audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_);
        audio_processor_initialized_ = true;
    }

    audio_processor_->EnableDeviceAec(enable);
}

void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks) {
    callbacks_ = callbacks;
}

void AudioService::SetProtocol(Protocol* protocol) {
    protocol_ = protocol;
}

void AudioService::PlaySound(const std::string_view& ogg) {
    if (!codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableOutput(true);
    }

    const uint8_t* buf = reinterpret_cast<const uint8_t*>(ogg.data());
    size_t size = ogg.size();
    size_t offset = 0;

    auto find_page = [&](size_t start)->size_t {
        for (size_t i = start; i + 4 <= size; ++i) {
            if (buf[i] == 'O' && buf[i+1] == 'g' && buf[i+2] == 'g' && buf[i+3] == 'S') return i;
        }
        return static_cast<size_t>(-1);
    };

    bool seen_head = false;
    bool seen_tags = false;
    int sample_rate = 16000; // 默认值

    while (true) {
        size_t pos = find_page(offset);
        if (pos == static_cast<size_t>(-1)) break;
        offset = pos;
        if (offset + 27 > size) break;

        const uint8_t* page = buf + offset;
        uint8_t page_segments = page[26];
        size_t seg_table_off = offset + 27;
        if (seg_table_off + page_segments > size) break;

        size_t body_size = 0;
        for (size_t i = 0; i < page_segments; ++i) body_size += page[27 + i];

        size_t body_off = seg_table_off + page_segments;
        if (body_off + body_size > size) break;

        // Parse packets using lacing
        size_t cur = body_off;
        size_t seg_idx = 0;
        while (seg_idx < page_segments) {
            size_t pkt_len = 0;
            size_t pkt_start = cur;
            bool continued = false;
            do {
                uint8_t l = page[27 + seg_idx++];
                pkt_len += l;
                cur += l;
                continued = (l == 255);
            } while (continued && seg_idx < page_segments);

            if (pkt_len == 0) continue;
            const uint8_t* pkt_ptr = buf + pkt_start;

            if (!seen_head) {
                // 解析OpusHead包
                if (pkt_len >= 19 && std::memcmp(pkt_ptr, "OpusHead", 8) == 0) {
                    seen_head = true;
                    
                    // OpusHead结构：[0-7] "OpusHead", [8] version, [9] channel_count, [10-11] pre_skip
                    // [12-15] input_sample_rate, [16-17] output_gain, [18] mapping_family
                    if (pkt_len >= 12) {
                        uint8_t version = pkt_ptr[8];
                        uint8_t channel_count = pkt_ptr[9];
                        
                        if (pkt_len >= 16) {
                            // 读取输入采样率 (little-endian)
                            sample_rate = pkt_ptr[12] | (pkt_ptr[13] << 8) | 
                                        (pkt_ptr[14] << 16) | (pkt_ptr[15] << 24);
                            ESP_LOGI(TAG, "OpusHead: version=%d, channels=%d, sample_rate=%d", 
                                   version, channel_count, sample_rate);
                        }
                    }
                }
                continue;
            }
            if (!seen_tags) {
                // Expect OpusTags in second packet
                if (pkt_len >= 8 && std::memcmp(pkt_ptr, "OpusTags", 8) == 0) {
                    seen_tags = true;
                }
                continue;
            }

            // Audio packet (Opus)
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = sample_rate;
            packet->frame_duration = 60;
            // 健壮性：限制每个Opus包大小，防止异常 OGG 造成巨大包
            if (pkt_len == 0 || pkt_len > MAX_OPUS_PAYLOAD_SIZE) {
                ESP_LOGE(TAG, "OGG packet length out of range: %u, skip", (unsigned)pkt_len);
            } else {
                packet->payload.resize(pkt_len);
                std::memcpy(packet->payload.data(), pkt_ptr, pkt_len);
                PushPacketToDecodeQueue(std::move(packet), true);
            }
        }

        offset = body_off + body_size;
    }
}

bool AudioService::IsIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return audio_encode_queue_.empty() && audio_decode_queue_.empty() && audio_playback_queue_.empty() && audio_testing_queue_.empty();
}

void AudioService::ResetDecoder() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    opus_decoder_->ResetState();
    timestamp_queue_.clear();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_testing_queue_.clear();
    audio_queue_cv_.notify_all();
}

void AudioService::CheckAndUpdateAudioPowerState() {
    auto now = std::chrono::steady_clock::now();
    auto input_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_input_time_).count();
    auto output_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_time_).count();
    if (input_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->input_enabled()) {
        codec_->EnableInput(false);
    }
    if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled()) {
        codec_->EnableOutput(false);
    }
    if (!codec_->input_enabled() && !codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
    }
}

void AudioService::SetModelsList(srmodel_list_t* models_list) {
    models_list_ = models_list;

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    if (esp_srmodel_filter(models_list_, ESP_MN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<CustomWakeWord>();
    } else if (esp_srmodel_filter(models_list_, ESP_WN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<AfeWakeWord>();
    } else {
        wake_word_ = nullptr;
    }
#else
    if (esp_srmodel_filter(models_list_, ESP_WN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<EspWakeWord>();
    } else {
        wake_word_ = nullptr;
    }
#endif

    if (wake_word_) {
        wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
            if (callbacks_.on_wake_word_detected) {
                callbacks_.on_wake_word_detected(wake_word);
            }
        });
    }
}

bool AudioService::IsAfeWakeWord() {
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    return wake_word_ != nullptr && dynamic_cast<AfeWakeWord*>(wake_word_.get()) != nullptr;
#else
    return false;
#endif
}
