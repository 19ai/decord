/*!
 *  Copyright (c) 2019 by Contributors if not otherwise specified
 * \file cuda_decoder.cc
 * \brief NVCUVID based decoder impl
 */

#include "cuda_threaded_decoder.h"
#include "cuda_mapped_frame.h"
#include "cuda_texture.h"
#include "../../improc/improc.h"
#include "nvcuvid/nvcuvid.h"
#include <nvml.h>


namespace decord {
namespace cuda {
using namespace runtime;

CUThreadedDecoder::CUThreadedDecoder(int device_id, AVCodecParameters *codecpar) 
    : device_id_(device_id), stream_({-1, false}), device_{}, ctx_{}, parser_{}, decoder_{}, 
    pkt_queue_{}, frame_queue_{}, buffer_queue_{}, reorder_buffer_{}, reorder_queue_(), frame_order_(), last_pts_(-1),
    permits_{}, run_(false), frame_count_(0), draining_(false),
    tex_registry_(), nv_time_base_({1, 10000000}), frame_base_({1, 1000000}),
    dec_ctx_(nullptr), bsf_ctx_(nullptr), width_(-1), height_(-1) {

    // initialize bitstream filters
    InitBitStreamFilter(codecpar);
    
    CHECK_CUDA_CALL(cuInit(0));
    CHECK_CUDA_CALL(cuDeviceGet(&device_, device_id_));

    char device_name[100];
    CHECK_CUDA_CALL(cuDeviceGetName(device_name, 100, device_));
    DLOG(INFO) << "Using device: " << device_name;

    try {
        auto nvml_ret = nvmlInit();
        if (nvml_ret != NVML_SUCCESS) {
            LOG(FATAL) << "nvmlInit returned error " << nvml_ret;
        }
        char nvmod_version_string[NVML_SYSTEM_DRIVER_VERSION_BUFFER_SIZE];
        nvml_ret = nvmlSystemGetDriverVersion(nvmod_version_string,
                                              sizeof(nvmod_version_string));
        if (nvml_ret != NVML_SUCCESS) {
            LOG(FATAL) << "nvmlSystemGetDriverVersion returned error " << nvml_ret;
        }
        auto nvmod_version = std::stof(nvmod_version_string);
        if (nvmod_version < 384.0f) {
            LOG(INFO) << "Older kernel module version " << nvmod_version
                        << " so using the default stream."
                        << std::endl;
            stream_ = CUStream(device_id_, true);
        } else {
            LOG(INFO) << "Kernel module version " << nvmod_version
                        << ", so using our own stream."
                        << std::endl;
        }
    } catch(const std::exception& e) {
        LOG(INFO) << "Unable to get nvidia kernel module version from NVML, "
                    << "conservatively assuming it is an older version.\n"
                    << "The error was: " << e.what()
                    << std::endl;
        stream_ = CUStream(device_id_, true);
    }

    ctx_ = CUContext(device_);
    if (!ctx_.Initialized()) {
        LOG(FATAL) << "Problem initializing context";
        return;
    }
}

void CUThreadedDecoder::InitBitStreamFilter(AVCodecParameters *codecpar) {
    const char* bsf_name = nullptr;
    if (AV_CODEC_ID_H264 == codecpar->codec_id) {
        // H.264
        bsf_name = "h264_mp4toannexb";
    } else if (AV_CODEC_ID_HEVC == codecpar->codec_id) {
        // HEVC
        bsf_name = "hevc_mp4toannexb";
    }

    auto bsf = av_bsf_get_by_name(bsf_name);
    CHECK(bsf) << "Error finding bitstream filter: " << bsf_name;

    AVBSFContext* bsf_ctx = nullptr;
    CHECK_GE(av_bsf_alloc(bsf, &bsf_ctx), 0) << "Error allocating bit stream filter context.";
    CHECK_GE(avcodec_parameters_copy(bsf_ctx->par_in, codecpar), 0) << "Error setting BSF parameters.";
    CHECK_GE(av_bsf_init(bsf_ctx), 0) << "Error init BSF";
    CHECK_GE(avcodec_parameters_copy(codecpar, bsf_ctx->par_out), 0) << "Error copy bsf output to codecpar";
    bsf_ctx_.reset(bsf_ctx);
}

void CUThreadedDecoder::SetCodecContext(AVCodecContext *dec_ctx, int width, int height) {
    CHECK(dec_ctx);
    LOG(INFO) << "SetCodecContext";
    width_ = width;
    height_ = height;
    bool running = run_.load();
    Clear();
    dec_ctx_.reset(dec_ctx);
    parser_ = CUVideoParser(dec_ctx->codec_id, this, kMaxOutputSurfaces, dec_ctx->extradata,
                            dec_ctx->extradata_size);
    if (!parser_.Initialized()) {
        LOG(FATAL) << "Problem creating video parser";
        return;
    }
    if (running) {
        Start();
    }
    LOG(INFO) << "Finish SetCOdecContext...";
}

void CUThreadedDecoder::Start() {
    if (run_.load()) return;

    LOG(INFO) << "Starting...";
    pkt_queue_.reset(new PacketQueue());
    frame_queue_.reset(new FrameQueue());
    buffer_queue_.reset(new BufferQueue());
    reorder_queue_.reset(new ReorderQueue());
    frame_order_.reset(new FrameOrderQueue());
    LOG(INFO) << "Reset done.";
    avcodec_flush_buffers(dec_ctx_.get());
    // frame_in_use_.resize(kMaxOutputSurfaces, 0);
    CHECK(permits_.size() == 0);
    LOG(INFO) << "resizing permits";
    permits_.resize(kMaxOutputSurfaces);
    LOG(INFO) << "init permits";
    for (auto& p : permits_) {
        p.reset(new PermitQueue());
        p->Push(1);
    }
    // LOG(INFO) << "permits initied.";
    run_.store(true);
    // launch worker threads
    LOG(INFO) << "launching workers";
    launcher_t_ = std::thread{&CUThreadedDecoder::LaunchThread, this};
    converter_t_ = std::thread{&CUThreadedDecoder::ConvertThread, this};
    LOG(INFO) << "finish launching workers";
}

void CUThreadedDecoder::Stop() {
    if (run_.load()) {
        pkt_queue_->SignalForKill();
        run_.store(false);
        frame_queue_->SignalForKill();
        buffer_queue_->SignalForKill();
        reorder_queue_->SignalForKill();
        frame_order_->SignalForKill();
        for (auto& p : permits_) {
            if (p) p->SignalForKill();
        }
        // frame_in_use_.clear();
    }
    if (launcher_t_.joinable()) {
        launcher_t_.join();
    }
    if (converter_t_.joinable()) {
        converter_t_.join();
    }
}

void CUThreadedDecoder::Clear() {
    Stop();
    frame_count_.store(0);
    reorder_buffer_.clear();
    for (auto& p : permits_) {
        if (p) p->SignalForKill();
    }
    permits_.clear();
    // frame_in_use_.clear();
}

CUThreadedDecoder::~CUThreadedDecoder() {
    Clear();
}

int CUDAAPI CUThreadedDecoder::HandlePictureSequence(void* user_data, CUVIDEOFORMAT* format) {
    auto decoder = reinterpret_cast<CUThreadedDecoder*>(user_data);
    // LOG(INFO) << "HandlePictureSequence, thread id: " << std::this_thread::get_id();;
    return decoder->HandlePictureSequence_(format);
}

int CUDAAPI CUThreadedDecoder::HandlePictureDecode(void* user_data,
                                            CUVIDPICPARAMS* pic_params) {
    auto decoder = reinterpret_cast<CUThreadedDecoder*>(user_data);
    // LOG(INFO) << "HandlePictureDecode, thread id: " << std::this_thread::get_id();;
    return decoder->HandlePictureDecode_(pic_params);
}

int CUDAAPI CUThreadedDecoder::HandlePictureDisplay(void* user_data,
                                             CUVIDPARSERDISPINFO* disp_info) {
    auto decoder = reinterpret_cast<CUThreadedDecoder*>(user_data);
    // LOG(INFO) << "HandlePictureDisplay, thread id: " << std::this_thread::get_id();;
    return decoder->HandlePictureDisplay_(disp_info);
}

int CUThreadedDecoder::HandlePictureSequence_(CUVIDEOFORMAT* format) {
    width_ = format->coded_width;
    height_ = format->coded_height;
    frame_base_ = {static_cast<int>(format->frame_rate.denominator),
                   static_cast<int>(format->frame_rate.numerator)};
    return decoder_.Initialize(format);
}

int CUThreadedDecoder::HandlePictureDecode_(CUVIDPICPARAMS* pic_params) {
    CHECK_GE(pic_params->CurrPicIdx, 0);
    CHECK_LT(pic_params->CurrPicIdx, permits_.size());
    auto permit_queue = permits_[pic_params->CurrPicIdx];
    int tmp;
    while (permit_queue->Size() < 1) continue;
    int ret = permit_queue->Pop(&tmp);
    if (!run_.load() || !ret) return 0;
    if (!CHECK_CUDA_CALL(cuvidDecodePicture(decoder_, pic_params))) {
        LOG(FATAL) << "Failed to launch cuvidDecodePicture";
        return 0;
    }
    // decoded_cnt_++;
    return 1;
}

int CUThreadedDecoder::HandlePictureDisplay_(CUVIDPARSERDISPINFO* disp_info) {
    // push to converter
    // LOG(INFO) << "frame in use occupy: " << disp_info->picture_index;
    // frame_in_use_[disp_info->picture_index] = 1;
    buffer_queue_->Push(disp_info);
    // finished, send clear msg to allow next decoding
    // LOG(INFO) << "finished, send clear msg to allow next decoding";
    return 1;
}

void CUThreadedDecoder::Push(AVPacketPtr pkt, NDArray buf) {
    CHECK(run_.load());
    if (!pkt) {
        CHECK(!draining_.load()) << "Start draining twice...";
        draining_.store(true);
    }
    // if (pkt) {
    //     // calculate frame number for output order
    //     auto frame_num = av_rescale_q(pkt->pts, AV_TIME_BASE_Q, dec_ctx_->time_base);
    //     frame_order_->Push(frame_num);
    // }
    if (pkt) {
        if (last_pts_ < 0) {
            last_pts_ = pkt->pts;
            frame_order_->Push(last_pts_);
        } else {
            last_pts_ += pkt->duration;
            frame_order_->Push(last_pts_);
        }
    }
    
    
    pkt_queue_->Push(pkt);
    frame_queue_->Push(buf);  // push memory buffer
    ++frame_count_;
}

bool CUThreadedDecoder::Pop(NDArray *frame) {
    if (!frame_count_.load() && !draining_.load()) {
        return false;
    }
    if (reorder_queue_->Size() < 1) {
        return false;
    }
    int ret = reorder_queue_->Pop(frame);
    if (!ret) return false;
    --frame_count_;
    return true;
}

void CUThreadedDecoder::LaunchThread() {
    ctx_.Push();
    // LOG(INFO) << "LaunchThread, thread id: " << std::this_thread::get_id();
    while (run_.load()) {
        bool ret;
        AVPacketPtr avpkt = nullptr;
        ret = pkt_queue_->Pop(&avpkt);
        if (!ret) return;

        if (avpkt && avpkt->size) {
            // bitstream filter raw packet
            AVPacketPtr filtered_avpkt = ffmpeg::AVPacketPool::Get()->Acquire();
            CHECK(av_bsf_send_packet(bsf_ctx_.get(), avpkt.get()) == 0) << "Error sending BSF packet";
            int bsf_ret;
            while ((bsf_ret = av_bsf_receive_packet(bsf_ctx_.get(), filtered_avpkt.get())) == 0) {
                CUVIDSOURCEDATAPACKET cupkt = {0};
                cupkt.payload_size = filtered_avpkt->size;
                cupkt.payload = filtered_avpkt->data;
                if (filtered_avpkt->pts != AV_NOPTS_VALUE) {
                    cupkt.flags = CUVID_PKT_TIMESTAMP;
                    cupkt.timestamp = filtered_avpkt->pts;
                }

                if (!CHECK_CUDA_CALL(cuvidParseVideoData(parser_, &cupkt))) {
                    LOG(FATAL) << "Problem decoding packet";
                }
            }
        } else {
            LOG(INFO) << "draining cu parser";
            CUVIDSOURCEDATAPACKET cupkt = {0};
            cupkt.flags = CUVID_PKT_ENDOFSTREAM;
            if (!CHECK_CUDA_CALL(cuvidParseVideoData(parser_, &cupkt))) {
                    LOG(FATAL) << "Problem decoding packet";
            }
            // mark as flushing?
        }
    }
}

void CUThreadedDecoder::ConvertThread() {
    ctx_.Push();
    // LOG(INFO) << "convert thread, thread id: " << std::this_thread::get_id();;
    while (run_.load()) {
        bool ret;
        CUVIDPARSERDISPINFO *disp_info = nullptr;
        NDArray arr;
        ret = buffer_queue_->Pop(&disp_info);
        if (!ret) return;
        CHECK(disp_info != nullptr);
        // CUDA mem buffer
        ret = frame_queue_->Pop(&arr);
        CHECK(arr.defined());
        // LOG(INFO) << "COnvert thread, ndarray buffer get";
        uint8_t* dst_ptr = static_cast<uint8_t*>(arr.data_->dl_tensor.data);
        auto frame = CUMappedFrame(disp_info, decoder_, stream_);
        // conversion to usable format, RGB, resize, etc...
        auto input_width = decoder_.Width();
        auto input_height = decoder_.Height();
        auto& textures = tex_registry_.GetTexture(frame.get_ptr(),
                                                  frame.get_pitch(),
                                                  input_width,
                                                  input_height,
                                                  ScaleMethod_Linear,
                                                  ChromaUpMethod_Linear);
        ProcessFrame(textures.chroma, textures.luma, dst_ptr, stream_, input_width, input_height, width_, height_);
        int64_t frame_pts = static_cast<int64_t>(frame.disp_info->timestamp);
        // auto frame_num = av_rescale_q(frame.disp_info->timestamp,
        //                               nv_time_base_, frame_base_);
        int64_t desired_pts;
        int fo_ret = frame_order_->Pop(&desired_pts);
        if (!fo_ret) return;
        if (desired_pts == frame_pts) {
            // queue top is current array
            reorder_queue_->Push(arr);
        } else {
            // store current arr to map
            reorder_buffer_[frame_pts] = arr;
            auto r = reorder_buffer_.find(static_cast<int64_t>(frame_pts));
            if (r != reorder_buffer_.end()) {
                // queue top is stored in map
                reorder_queue_->Push(r->second);
                reorder_buffer_.erase(r);
            }
        }

        // Output cleared, allow next decoding
        permits_[disp_info->picture_index]->Push(1);
    }
}

}  // namespace cuda
}  // namespace decord