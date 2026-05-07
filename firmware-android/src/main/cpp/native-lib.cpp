#include <jni.h>
#include <android/log.h>
#include <memory>
#include <mutex>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include "ButterworthFilter.hpp"

#define TAG "FusionNetNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace {
    std::unique_ptr<FusionNet::ButterworthFilter> g_filter;
    std::mutex g_filter_mutex;
    
    // Memory-mapped buffer structure
    struct MappedBuffer {
        int fd;
        double* data;
        size_t size;
        size_t capacity;
        bool is_mapped;
        
        MappedBuffer() : fd(-1), data(nullptr), size(0), capacity(0), is_mapped(false) {}
    };
    
    MappedBuffer g_input_buffer;
    MappedBuffer g_output_buffer;
}

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_fusionnet_NativeFilter_createFilter(JNIEnv* env, jobject thiz) {
    std::lock_guard<std::mutex> lock(g_filter_mutex);
    
    try {
        g_filter = std::make_unique<FusionNet::ButterworthFilter>();
        if (!g_filter->initialize()) {
            LOGE("Failed to initialize Butterworth filter");
            g_filter.reset();
            return 0;
        }
        
        LOGI("Butterworth filter created successfully");
        return reinterpret_cast<jlong>(g_filter.get());
    } catch (const std::exception& e) {
        LOGE("Exception creating filter: %s", e.what());
        return 0;
    }
}

JNIEXPORT void JNICALL
Java_com_fusionnet_NativeFilter_destroyFilter(JNIEnv* env, jobject thiz, jlong filter_ptr) {
    std::lock_guard<std::mutex> lock(g_filter_mutex);
    
    if (g_filter && reinterpret_cast<jlong>(g_filter.get()) == filter_ptr) {
        g_filter.reset();
        LOGI("Butterworth filter destroyed");
    }
}

JNIEXPORT jdouble JNICALL
Java_com_fusionnet_NativeFilter_filterSample(JNIEnv* env, jobject thiz, jlong filter_ptr, jdouble input) {
    std::lock_guard<std::mutex> lock(g_filter_mutex);
    
    if (!g_filter || reinterpret_cast<jlong>(g_filter.get()) != filter_ptr) {
        LOGE("Invalid filter pointer");
        return input;
    }
    
    return g_filter->filterSample(static_cast<double>(input));
}

JNIEXPORT jint JNICALL
Java_com_fusionnet_NativeFilter_filterBuffer(JNIEnv* env, jobject thiz, jlong filter_ptr,
                                            jdoubleArray input, jdoubleArray output, jint length) {
    std::lock_guard<std::mutex> lock(g_filter_mutex);
    
    if (!g_filter || reinterpret_cast<jlong>(g_filter.get()) != filter_ptr) {
        LOGE("Invalid filter pointer");
        return -1;
    }
    
    if (!input || !output || length <= 0) {
        LOGE("Invalid parameters");
        return -1;
    }
    
    jsize input_length = env->GetArrayLength(input);
    jsize output_length = env->GetArrayLength(output);
    
    if (input_length < length || output_length < length) {
        LOGE("Buffer length insufficient");
        return -1;
    }
    
    jdouble* input_ptr = env->GetDoubleArrayElements(input, nullptr);
    jdouble* output_ptr = env->GetDoubleArrayElements(output, nullptr);
    
    if (!input_ptr || !output_ptr) {
        LOGE("Failed to get array elements");
        if (input_ptr) env->ReleaseDoubleArrayElements(input, input_ptr, JNI_ABORT);
        if (output_ptr) env->ReleaseDoubleArrayElements(output, output_ptr, JNI_ABORT);
        return -1;
    }
    
    g_filter->filterBuffer(reinterpret_cast<double*>(input_ptr), 
                          reinterpret_cast<double*>(output_ptr), 
                          static_cast<size_t>(length));
    
    env->ReleaseDoubleArrayElements(input, input_ptr, JNI_ABORT);
    env->ReleaseDoubleArrayElements(output, output_ptr, 0);
    
    return length;
}

JNIEXPORT jint JNICALL
Java_com_fusionnet_NativeFilter_createMappedBuffer(JNIEnv* env, jobject thiz, jstring name, jint capacity) {
    const char* buffer_name = env->GetStringUTFChars(name, nullptr);
    if (!buffer_name) {
        LOGE("Failed to get buffer name");
        return -1;
    }
    
    // Create shared memory file
    std::string shm_path = "/dev/shm/";
    shm_path += buffer_name;
    
    int fd = shm_open(buffer_name, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd == -1) {
        LOGE("Failed to create shared memory: %s", strerror(errno));
        env->ReleaseStringUTFChars(name, buffer_name);
        return -1;
    }
    
    // Set the size of the shared memory
    size_t buffer_size = capacity * sizeof(double);
    if (ftruncate(fd, buffer_size) == -1) {
        LOGE("Failed to truncate shared memory: %s", strerror(errno));
        close(fd);
        shm_unlink(buffer_name);
        env->ReleaseStringUTFChars(name, buffer_name);
        return -1;
    }
    
    // Map the memory
    void* mapped = mmap(nullptr, buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        LOGE("Failed to map shared memory: %s", strerror(errno));
        close(fd);
        shm_unlink(buffer_name);
        env->ReleaseStringUTFChars(name, buffer_name);
        return -1;
    }
    
    // Store buffer info based on name
    MappedBuffer* buffer = nullptr;
    if (strstr(buffer_name, "input")) {
        buffer = &g_input_buffer;
    } else if (strstr(buffer_name, "output")) {
        buffer = &g_output_buffer;
    } else {
        LOGE("Unknown buffer name pattern");
        munmap(mapped, buffer_size);
        close(fd);
        shm_unlink(buffer_name);
        env->ReleaseStringUTFChars(name, buffer_name);
        return -1;
    }
    
    // Clean up existing buffer if any
    if (buffer->is_mapped) {
        munmap(buffer->data, buffer->capacity * sizeof(double));
        close(buffer->fd);
    }
    
    buffer->fd = fd;
    buffer->data = static_cast<double*>(mapped);
    buffer->capacity = capacity;
    buffer->size = 0;
    buffer->is_mapped = true;
    
    LOGI("Created mapped buffer '%s' with capacity %d", buffer_name, capacity);
    env->ReleaseStringUTFChars(name, buffer_name);
    
    return 0;
}

JNIEXPORT void JNICALL
Java_com_fusionnet_NativeFilter_writeToBuffer(JNIEnv* env, jobject thiz, jstring name, 
                                             jdoubleArray data, jint offset, jint length) {
    const char* buffer_name = env->GetStringUTFChars(name, nullptr);
    if (!buffer_name) {
        LOGE("Failed to get buffer name");
        return;
    }
    
    MappedBuffer* buffer = nullptr;
    if (strstr(buffer_name, "input")) {
        buffer = &g_input_buffer;
    } else if (strstr(buffer_name, "output")) {
        buffer = &g_output_buffer;
    } else {
        LOGE("Unknown buffer name pattern");
        env->ReleaseStringUTFChars(name, buffer_name);
        return;
    }
    
    if (!buffer->is_mapped || !buffer->data) {
        LOGE("Buffer not mapped");
        env->ReleaseStringUTFChars(name, buffer_name);
        return;
    }
    
    jsize data_length = env->GetArrayLength(data);
    if (offset < 0 || length <= 0 || offset + length > data_length) {
        LOGE("Invalid offset or length");
        env->ReleaseStringUTFChars(name, buffer_name);
        return;
    }
    
    if (buffer->size + static_cast<size_t>(length) > buffer->capacity) {
        LOGE("Buffer overflow");
        env->ReleaseStringUTFChars(name, buffer_name);
        return;
    }
    
    jdouble* data_ptr = env->GetDoubleArrayElements(data, nullptr);
    if (!data_ptr) {
        LOGE("Failed to get array elements");
        env->ReleaseStringUTFChars(name, buffer_name);
        return;
    }
    
    // Copy data to mapped buffer
    memcpy(buffer->data + buffer->size, data_ptr + offset, length * sizeof(double));
    buffer->size += length;
    
    env->ReleaseDoubleArrayElements(data, data_ptr, JNI_ABORT);
    env->ReleaseStringUTFChars(name, buffer_name);
    
    LOGI("Wrote %d samples to buffer '%s'", length, buffer_name);
}

JNIEXPORT jint JNICALL
Java_com_fusionnet_NativeFilter_processMappedBuffers(JNIEnv* env, jobject thiz, jlong filter_ptr) {
    std::lock_guard<std::mutex> lock(g_filter_mutex);
    
    if (!g_filter || reinterpret_cast<jlong>(g_filter.get()) != filter_ptr) {
        LOGE("Invalid filter pointer");
        return -1;
    }
    
    if (!g_input_buffer.is_mapped || !g_output_buffer.is_mapped) {
        LOGE("Buffers not properly mapped");
        return -1;
    }
    
    if (g_input_buffer.size == 0) {
        LOGI("No data to process");
        return 0;
    }
    
    // Create filter buffer structure
    FusionNet::ButterworthFilter::FilterBuffer filter_buffer;
    filter_buffer.input_data = g_input_buffer.data;
    filter_buffer.output_data = g_output_buffer.data;
    filter_buffer.length = g_input_buffer.size;
    filter_buffer.processed_samples = 0;
    
    // Process the buffer
    size_t processed = g_filter->processMappedBuffer(&filter_buffer);
    
    // Update output buffer size
    g_output_buffer.size = processed;
    
    LOGI("Processed %zu samples from mapped buffers", processed);
    return static_cast<jint>(processed);
}

JNIEXPORT void JNICALL
Java_com_fusionnet_NativeFilter_resetBuffers(JNIEnv* env, jobject thiz) {
    g_input_buffer.size = 0;
    g_output_buffer.size = 0;
    
    if (g_input_buffer.data) {
        memset(g_input_buffer.data, 0, g_input_buffer.capacity * sizeof(double));
    }
    
    if (g_output_buffer.data) {
        memset(g_output_buffer.data, 0, g_output_buffer.capacity * sizeof(double));
    }
    
    LOGI("Reset mapped buffers");
}

JNIEXPORT void JNICALL
Java_com_fusionnet_NativeFilter_destroyBuffers(JNIEnv* env, jobject thiz) {
    // Cleanup input buffer
    if (g_input_buffer.is_mapped && g_input_buffer.data) {
        munmap(g_input_buffer.data, g_input_buffer.capacity * sizeof(double));
        close(g_input_buffer.fd);
        g_input_buffer.is_mapped = false;
    }
    
    // Cleanup output buffer
    if (g_output_buffer.is_mapped && g_output_buffer.data) {
        munmap(g_output_buffer.data, g_output_buffer.capacity * sizeof(double));
        close(g_output_buffer.fd);
        g_output_buffer.is_mapped = false;
    }
    
    LOGI("Destroyed mapped buffers");
}

JNIEXPORT jdouble JNICALL
Java_com_fusionnet_NativeFilter_getFilterSpec(JNIEnv* env, jobject thiz, jint spec_type) {
    if (!g_filter) {
        return -1.0;
    }
    
    switch (spec_type) {
        case 0: return g_filter->getCutOffFrequency();
        case 1: return g_filter->getSamplingFrequency();
        case 2: return static_cast<double>(g_filter->getOrder());
        default: return -1.0;
    }
}

} // extern "C"
