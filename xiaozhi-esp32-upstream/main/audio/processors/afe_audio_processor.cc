#include "afe_audio_processor.h"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#define PROCESSOR_RUNNING 0x01

#define TAG "AfeAudioProcessor"
static constexpr uint32_t kAudioProcessorTaskStackSize = 4096;

AfeAudioProcessor::AfeAudioProcessor()
    : afe_data_(nullptr) {
    event_group_ = xEventGroupCreate();
}

bool AfeAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    if (audio_task_handle_ != nullptr && afe_data_ != nullptr) {
        ESP_LOGI(TAG,
                 "Initialize skipped: afe_data=%p task_handle=%p feed_size=%u",
                 afe_data_,
                 audio_task_handle_,
                 static_cast<unsigned>(afe_iface_->get_feed_chunksize(afe_data_)));
        return true;
    }
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
        afe_data_ = nullptr;
        afe_iface_ = nullptr;
    }

    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;

    // Pre-allocate output buffer capacity
    output_buffer_.reserve(frame_samples_);

    int ref_num = codec_->input_reference() ? 1 : 0;

    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; i++) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; i++) {
        input_format.push_back('R');
    }

    srmodel_list_t *models;
    if (models_list == nullptr) {
        models = esp_srmodel_init("model");
    } else {
        models = models_list;
    }

    char* ns_model_name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL);
    char* vad_model_name = esp_srmodel_filter(models, ESP_VADN_PREFIX, NULL);
    
    afe_config_t* afe_config = afe_config_init(input_format.c_str(), NULL, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
    afe_config->vad_mode = VAD_MODE_0;
    afe_config->vad_min_noise_ms = 100;
    if (vad_model_name != nullptr) {
        afe_config->vad_model_name = vad_model_name;
    }

    if (ns_model_name != nullptr) {
        afe_config->ns_init = true;
        afe_config->ns_model_name = ns_model_name;
        afe_config->afe_ns_mode = AFE_NS_MODE_NET;
    } else {
        afe_config->ns_init = false;
    }

    afe_config->agc_init = false;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

#ifdef CONFIG_USE_DEVICE_AEC
    afe_config->aec_init = true;
    afe_config->vad_init = false;
#else
    afe_config->aec_init = false;
    afe_config->vad_init = true;
#endif

    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t min_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largest_spiram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG,
             "Initialize: frame_duration_ms=%d frame_samples=%d input_channels=%d ref_num=%d input_format=%s vad_model=%s ns_model=%s device_aec=%d free_internal=%u min_internal=%u largest_internal=%u free_psram=%u largest_psram=%u",
             frame_duration_ms,
             frame_samples_,
             codec_->input_channels(),
             ref_num,
             input_format.c_str(),
             vad_model_name != nullptr ? vad_model_name : "(null)",
             ns_model_name != nullptr ? ns_model_name : "(null)",
#ifdef CONFIG_USE_DEVICE_AEC
             1,
#else
             0,
#endif
             static_cast<unsigned>(free_internal),
             static_cast<unsigned>(min_internal),
             static_cast<unsigned>(largest_internal),
             static_cast<unsigned>(free_spiram),
             static_cast<unsigned>(largest_spiram)
    );

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    if (afe_iface_ == nullptr) {
        ESP_LOGE(TAG, "Initialize failed: esp_afe_handle_from_config returned null");
        return false;
    }

    afe_data_ = afe_iface_->create_from_config(afe_config);
    ESP_LOGI(TAG,
             "Initialize: afe_iface=%p afe_data=%p feed_size=%u fetch_size=%u",
             afe_iface_,
             afe_data_,
             afe_data_ != nullptr ? static_cast<unsigned>(afe_iface_->get_feed_chunksize(afe_data_)) : 0U,
             afe_data_ != nullptr ? static_cast<unsigned>(afe_iface_->get_fetch_chunksize(afe_data_)) : 0U);
    if (afe_data_ == nullptr) {
        ESP_LOGE(TAG, "Initialize failed: create_from_config returned null");
        afe_iface_ = nullptr;
        return false;
    }

    if (audio_task_stack_ == nullptr) {
        audio_task_stack_ = static_cast<StackType_t*>(heap_caps_malloc(kAudioProcessorTaskStackSize, MALLOC_CAP_SPIRAM));
    }
    if (audio_task_buffer_ == nullptr) {
        audio_task_buffer_ = static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
    }
    ESP_LOGI(TAG,
             "Initialize: task_stack=%p task_buffer=%p stack_size=%u",
             audio_task_stack_,
             audio_task_buffer_,
             static_cast<unsigned>(kAudioProcessorTaskStackSize));
    if (audio_task_stack_ == nullptr || audio_task_buffer_ == nullptr) {
        ESP_LOGE(TAG,
                 "Initialize failed: unable to allocate task memory (stack=%p buffer=%p)",
                 audio_task_stack_,
                 audio_task_buffer_);
        afe_iface_->destroy(afe_data_);
        afe_data_ = nullptr;
        afe_iface_ = nullptr;
        return false;
    }

    audio_task_handle_ = xTaskCreateStatic([](void* arg) {
        auto this_ = (AfeAudioProcessor*)arg;
        this_->AudioProcessorTask();
        vTaskDelete(NULL);
    }, "audio_communication", kAudioProcessorTaskStackSize, this, 3, audio_task_stack_, audio_task_buffer_);
    ESP_LOGI(TAG,
             "Initialize: xTaskCreateStatic task_handle=%p",
             audio_task_handle_);
    if (audio_task_handle_ == nullptr) {
        ESP_LOGE(TAG, "Initialize failed: unable to create audio communication task");
        afe_iface_->destroy(afe_data_);
        afe_data_ = nullptr;
        afe_iface_ = nullptr;
        return false;
    }

    return true;
}

AfeAudioProcessor::~AfeAudioProcessor() {
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }
    if (audio_task_handle_ == nullptr) {
        if (audio_task_stack_ != nullptr) {
            heap_caps_free(audio_task_stack_);
        }
        if (audio_task_buffer_ != nullptr) {
            heap_caps_free(audio_task_buffer_);
        }
    }
    vEventGroupDelete(event_group_);
}

size_t AfeAudioProcessor::GetFeedSize() {
    if (afe_data_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeAudioProcessor::Feed(std::vector<int16_t>&& data) {
    if (afe_data_ == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    // Check running state inside lock to avoid TOCTOU race with Stop()
    if (!IsRunning()) {
        return;
    }
    input_buffer_.insert(input_buffer_.end(), data.begin(), data.end());
    size_t chunk_size = afe_iface_->get_feed_chunksize(afe_data_) * codec_->input_channels();
    while (input_buffer_.size() >= chunk_size) {
        afe_iface_->feed(afe_data_, input_buffer_.data());
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + chunk_size);
    }
}

void AfeAudioProcessor::Start() {
    ESP_LOGI(TAG,
             "Start: afe_data=%p task_handle=%p running_before=%d",
             afe_data_,
             audio_task_handle_,
             IsRunning() ? 1 : 0);
    if (afe_data_ == nullptr || audio_task_handle_ == nullptr) {
        ESP_LOGE(TAG,
                 "Start skipped: afe_data=%p task_handle=%p",
                 afe_data_,
                 audio_task_handle_);
        return;
    }
    xEventGroupSetBits(event_group_, PROCESSOR_RUNNING);
    ESP_LOGI(TAG, "Start done: running_after=%d", IsRunning() ? 1 : 0);
}

void AfeAudioProcessor::Stop() {
    ESP_LOGI(TAG,
             "Stop: afe_data=%p task_handle=%p running_before=%d input_buffer_samples=%u",
             afe_data_,
             audio_task_handle_,
             IsRunning() ? 1 : 0,
             static_cast<unsigned>(input_buffer_.size()));
    xEventGroupClearBits(event_group_, PROCESSOR_RUNNING);

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    if (afe_data_ != nullptr) {
        afe_iface_->reset_buffer(afe_data_);
    }
    input_buffer_.clear();
}

bool AfeAudioProcessor::IsRunning() {
    return xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING;
}

void AfeAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = callback;
}

void AfeAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = callback;
}

void AfeAudioProcessor::AudioProcessorTask() {
    if (afe_iface_ == nullptr || afe_data_ == nullptr) {
        ESP_LOGE(TAG,
                 "Audio communication task aborted: afe_iface=%p afe_data=%p",
                 afe_iface_,
                 afe_data_);
        return;
    }

    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    int64_t stats_window_start_us = esp_timer_get_time();
    int64_t last_fetch_us = 0;
    uint32_t fetch_count = 0;
    uint32_t fetch_fail_count = 0;
    uint32_t vad_speech_count = 0;
    uint32_t output_frames = 0;
    size_t output_samples = 0;
    ESP_LOGI(TAG, "Audio communication task started, feed size: %d fetch size: %d",
        feed_size, fetch_size);

    while (true) {
        xEventGroupWaitBits(event_group_, PROCESSOR_RUNNING, pdFALSE, pdTRUE, portMAX_DELAY);

        auto res = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
        if ((xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING) == 0) {
            continue;
        }
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            fetch_fail_count++;
            if (res != nullptr) {
                ESP_LOGI(TAG, "Error code: %d", res->ret_value);
            }
            continue;
        }
        int64_t now_us = esp_timer_get_time();
        if (last_fetch_us == 0) {
            stats_window_start_us = now_us;
        }
        last_fetch_us = now_us;
        fetch_count++;
        if (res->vad_state == VAD_SPEECH) {
            vad_speech_count++;
        }

        // VAD state change
        if (vad_state_change_callback_) {
            if (res->vad_state == VAD_SPEECH && !is_speaking_) {
                is_speaking_ = true;
                vad_state_change_callback_(true);
            } else if (res->vad_state == VAD_SILENCE && is_speaking_) {
                is_speaking_ = false;
                vad_state_change_callback_(false);
            }
        }

        if (output_callback_) {
            size_t samples = res->data_size / sizeof(int16_t);
            output_samples += samples;
            
            // Add data to buffer
            output_buffer_.insert(output_buffer_.end(), res->data, res->data + samples);
            
            // Output complete frames when buffer has enough data
            while (output_buffer_.size() >= frame_samples_) {
                if (output_buffer_.size() == frame_samples_) {
                    // If buffer size equals frame size, move the entire buffer
                    output_frames++;
                    output_callback_(std::move(output_buffer_));
                    output_buffer_.clear();
                    output_buffer_.reserve(frame_samples_);
                } else {
                    // If buffer size exceeds frame size, copy one frame and remove it
                    output_frames++;
                    output_callback_(std::vector<int16_t>(output_buffer_.begin(), output_buffer_.begin() + frame_samples_));
                    output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
                }
            }
        }

        if ((now_us - stats_window_start_us) >= 1000 * 1000LL) {
            int64_t window_us = now_us - stats_window_start_us;
            uint32_t avg_fetch_interval_ms = fetch_count > 0
                ? static_cast<uint32_t>((window_us / fetch_count) / 1000)
                : 0;
            ESP_LOGI(TAG,
                "AFE fetch stats: fetches=%u fetch_fail=%u avg_fetch_interval_ms=%u vad_speech=%u output_frames=%u output_samples=%u output_buffer_samples=%u running=%d",
                fetch_count,
                fetch_fail_count,
                avg_fetch_interval_ms,
                vad_speech_count,
                output_frames,
                static_cast<unsigned>(output_samples),
                static_cast<unsigned>(output_buffer_.size()),
                IsRunning() ? 1 : 0);
            stats_window_start_us = now_us;
            fetch_count = 0;
            fetch_fail_count = 0;
            vad_speech_count = 0;
            output_frames = 0;
            output_samples = 0;
        }
    }
}

void AfeAudioProcessor::EnableDeviceAec(bool enable) {
    if (enable) {
#if CONFIG_USE_DEVICE_AEC
        afe_iface_->disable_vad(afe_data_);
        afe_iface_->enable_aec(afe_data_);
#else
        ESP_LOGE(TAG, "Device AEC is not supported");
#endif
    } else {
        afe_iface_->disable_aec(afe_data_);
        afe_iface_->enable_vad(afe_data_);
    }
}
