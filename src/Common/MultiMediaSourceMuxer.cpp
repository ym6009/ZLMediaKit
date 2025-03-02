﻿/*
* Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
*
* This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
*
* Use of this source code is governed by MIT license that can be found in the
* LICENSE file in the root of the source tree. All contributing project authors
* may be found in the AUTHORS file in the root of the source tree.
*/

#include <math.h>
#include "Common/config.h"
#include "MultiMediaSourceMuxer.h"

using namespace std;
using namespace toolkit;

namespace toolkit {
    StatisticImp(mediakit::MultiMediaSourceMuxer);
}

namespace mediakit {

namespace {
class MediaSourceForMuxer : public MediaSource {
public:
    MediaSourceForMuxer(const MultiMediaSourceMuxer::Ptr &muxer)
        : MediaSource("muxer", muxer->getVhost(), muxer->getApp(), muxer->getStreamId()) {
        MediaSource::setListener(muxer);
    }
    int readerCount() override { return 0; }
};
} // namespace

static std::shared_ptr<MediaSinkInterface> makeRecorder(MediaSource &sender, const vector<Track::Ptr> &tracks, Recorder::type type, const ProtocolOption &option){
    auto recorder = Recorder::createRecorder(type, sender.getVhost(), sender.getApp(), sender.getId(), option);
    for (auto &track : tracks) {
        recorder->addTrack(track);
    }
    return recorder;
}

static string getTrackInfoStr(const TrackSource *track_src){
    _StrPrinter codec_info;
    auto tracks = track_src->getTracks(true);
    for (auto &track : tracks) {
        auto codec_type = track->getTrackType();
        codec_info << track->getCodecName();
        switch (codec_type) {
            case TrackAudio : {
                auto audio_track = dynamic_pointer_cast<AudioTrack>(track);
                codec_info << "["
                           << audio_track->getAudioSampleRate() << "/"
                           << audio_track->getAudioChannel() << "/"
                           << audio_track->getAudioSampleBit() << "] ";
                break;
            }
            case TrackVideo : {
                auto video_track = dynamic_pointer_cast<VideoTrack>(track);
                codec_info << "["
                           << video_track->getVideoWidth() << "/"
                           << video_track->getVideoHeight() << "/"
                           << round(video_track->getVideoFps()) << "] ";
                break;
            }
            default:
                break;
        }
    }
    return std::move(codec_info);
}

const std::string &MultiMediaSourceMuxer::getVhost() const {
    return _vhost;
}

const std::string &MultiMediaSourceMuxer::getApp() const {
    return _app;
}

const std::string &MultiMediaSourceMuxer::getStreamId() const {
    return _stream_id;
}

std::string MultiMediaSourceMuxer::shortUrl() const {
    auto ret = getOriginUrl(MediaSource::NullMediaSource());
    if (!ret.empty()) {
        return ret;
    }
    return _vhost + "/" + _app + "/" + _stream_id;
}

MultiMediaSourceMuxer::MultiMediaSourceMuxer(const string &vhost, const string &app, const string &stream, float dur_sec, const ProtocolOption &option) {
    _poller = EventPollerPool::Instance().getPoller();
    _create_in_poller = _poller->isCurrentThread();
    _vhost = vhost;
    _app = app;
    _stream_id = stream;
    _option = option;

    if (option.enable_rtmp) {
        _rtmp = std::make_shared<RtmpMediaSourceMuxer>(vhost, app, stream, option, std::make_shared<TitleMeta>(dur_sec));
    }
    if (option.enable_rtsp) {
        _rtsp = std::make_shared<RtspMediaSourceMuxer>(vhost, app, stream, option, std::make_shared<TitleSdp>(dur_sec));
    }
    if (option.enable_hls) {
        _hls = dynamic_pointer_cast<HlsRecorder>(Recorder::createRecorder(Recorder::type_hls, vhost, app, stream, option));
    }
    if (option.enable_mp4) {
        _mp4 = Recorder::createRecorder(Recorder::type_mp4, vhost, app, stream, option);
    }
    if (option.enable_ts) {
        _ts = std::make_shared<TSMediaSourceMuxer>(vhost, app, stream, option);
    }
#if defined(ENABLE_MP4)
    if (option.enable_fmp4) {
        _fmp4 = std::make_shared<FMP4MediaSourceMuxer>(vhost, app, stream, option);
    }
#endif

    //音频相关设置
    enableAudio(option.enable_audio);
    enableMuteAudio(option.add_mute_audio);
}

void MultiMediaSourceMuxer::setMediaListener(const std::weak_ptr<MediaSourceEvent> &listener) {
    setDelegate(listener);

    auto self = shared_from_this();
    //拦截事件
    if (_rtmp) {
        _rtmp->setListener(self);
    }
    if (_rtsp) {
        _rtsp->setListener(self);
    }
    if (_ts) {
        _ts->setListener(self);
    }
#if defined(ENABLE_MP4)
    if (_fmp4) {
        _fmp4->setListener(self);
    }
#endif
    auto hls = _hls;
    if (hls) {
        hls->setListener(self);
    }
}

void MultiMediaSourceMuxer::setTrackListener(const std::weak_ptr<Listener> &listener) {
    _track_listener = listener;
}

int MultiMediaSourceMuxer::totalReaderCount() const {
    auto hls = _hls;
    return (_rtsp ? _rtsp->readerCount() : 0) +
           (_rtmp ? _rtmp->readerCount() : 0) +
           (_ts ? _ts->readerCount() : 0) +
           #if defined(ENABLE_MP4)
           (_fmp4 ? _fmp4->readerCount() : 0) +
           #endif
           (_mp4 ? _option.mp4_as_player : 0) +
           (hls ? hls->readerCount() : 0) +
           (_ring ? _ring->readerCount() : 0);
}

void MultiMediaSourceMuxer::setTimeStamp(uint32_t stamp) {
    if (_rtmp) {
        _rtmp->setTimeStamp(stamp);
    }
    if (_rtsp) {
        _rtsp->setTimeStamp(stamp);
    }
}

int MultiMediaSourceMuxer::totalReaderCount(MediaSource &sender) {
    auto listener = getDelegate();
    if (!listener) {
        return totalReaderCount();
    }
    try {
        return listener->totalReaderCount(sender);
    } catch (MediaSourceEvent::NotImplemented &) {
        //listener未重载totalReaderCount
        return totalReaderCount();
    }
}

//此函数可能跨线程调用
bool MultiMediaSourceMuxer::setupRecord(MediaSource &sender, Recorder::type type, bool start, const string &custom_path, size_t max_second) {
    onceToken token(nullptr, [&]() {
        if (_option.mp4_as_player && type == Recorder::type_mp4) {
            //开启关闭mp4录制，触发观看人数变化相关事件
            onReaderChanged(sender, totalReaderCount());
        }
    });
    switch (type) {
        case Recorder::type_hls : {
            if (start && !_hls) {
                //开始录制
                _option.hls_save_path = custom_path;
                auto hls = dynamic_pointer_cast<HlsRecorder>(makeRecorder(sender, getTracks(), type, _option));
                if (hls) {
                    //设置HlsMediaSource的事件监听器
                    hls->setListener(shared_from_this());
                }
                _hls = hls;
            } else if (!start && _hls) {
                //停止录制
                _hls = nullptr;
            }
            return true;
        }
        case Recorder::type_mp4 : {
            if (start && !_mp4) {
                //开始录制
                _option.mp4_save_path = custom_path;
                _option.mp4_max_second = max_second;
                _mp4 = makeRecorder(sender, getTracks(), type, _option);
            } else if (!start && _mp4) {
                //停止录制
                _mp4 = nullptr;
            }
            return true;
        }
        default : return false;
    }
}

//此函数可能跨线程调用
bool MultiMediaSourceMuxer::isRecording(MediaSource &sender, Recorder::type type) {
    switch (type){
        case Recorder::type_hls :
            return !!_hls;
        case Recorder::type_mp4 :
            return !!_mp4;
        default:
            return false;
    }
}

void MultiMediaSourceMuxer::startSendRtp(MediaSource &sender, const MediaSourceEvent::SendRtpArgs &args, const std::function<void(uint16_t, const toolkit::SockException &)> cb) {
#if defined(ENABLE_RTPPROXY)
    createGopCacheIfNeed();

    auto ring = _ring;
    auto ssrc = args.ssrc;
    auto tracks = getTracks(false);
    auto rtp_sender = std::make_shared<RtpSender>(getOwnerPoller(sender));
    weak_ptr<MultiMediaSourceMuxer> weak_self = shared_from_this();

    rtp_sender->startSend(args, [ssrc, weak_self, rtp_sender, cb, tracks, ring](uint16_t local_port, const SockException &ex) mutable {
        cb(local_port, ex);
        auto strong_self = weak_self.lock();
        if (!strong_self || ex) {
            return;
        }

        for (auto &track : tracks) {
            rtp_sender->addTrack(track);
        }
        rtp_sender->addTrackCompleted();
        rtp_sender->setOnClose([weak_self, ssrc](const toolkit::SockException &ex) {
            if (auto strong_self = weak_self.lock()) {
                // 可能归属线程发生变更
                strong_self->getOwnerPoller(MediaSource::NullMediaSource())->async([=]() {
                    WarnL << "stream:" << strong_self->shortUrl() << " stop send rtp:" << ssrc << ", reason:" << ex.what();
                    strong_self->_rtp_sender.erase(ssrc);
                    NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastSendRtpStopped, *strong_self, ssrc, ex);
                });
            }
        });

        auto reader = ring->attach(EventPoller::getCurrentPoller());
        reader->setReadCB([rtp_sender](const Frame::Ptr &frame) {
            rtp_sender->inputFrame(frame);
        });

        // 可能归属线程发生变更
        strong_self->getOwnerPoller(MediaSource::NullMediaSource())->async([=]() {
            strong_self->_rtp_sender[ssrc] = std::move(reader);
        });
    });
#else
    cb(0, SockException(Err_other, "该功能未启用，编译时请打开ENABLE_RTPPROXY宏"));
#endif//ENABLE_RTPPROXY
}

bool MultiMediaSourceMuxer::stopSendRtp(MediaSource &sender, const string &ssrc) {
#if defined(ENABLE_RTPPROXY)
    if (ssrc.empty()) {
        //关闭全部
        auto size = _rtp_sender.size();
        _rtp_sender.clear();
        return size;
    }
    //关闭特定的
    return _rtp_sender.erase(ssrc);
#else
    return false;
#endif//ENABLE_RTPPROXY
}

vector<Track::Ptr> MultiMediaSourceMuxer::getMediaTracks(MediaSource &sender, bool trackReady) const {
    return getTracks(trackReady);
}

EventPoller::Ptr MultiMediaSourceMuxer::getOwnerPoller(MediaSource &sender) {
    auto listener = getDelegate();
    if (!listener) {
        return _poller;
    }
    try {
        auto ret = listener->getOwnerPoller(sender);
        if (ret != _poller) {
            WarnL << "OwnerPoller changed:" << shortUrl();
            _poller = ret;
        }
        return ret;
    } catch (MediaSourceEvent::NotImplemented &) {
        // listener未重载getOwnerPoller
        return _poller;
    }
}

bool MultiMediaSourceMuxer::onTrackReady(const Track::Ptr &track) {

    bool ret = false;
    if (_rtmp) {
        ret = _rtmp->addTrack(track) ? true : ret;
    }
    if (_rtsp) {
        ret = _rtsp->addTrack(track) ? true : ret;
    }
    if (_ts) {
        ret = _ts->addTrack(track) ? true : ret;
    }
#if defined(ENABLE_MP4)
    if (_fmp4) {
        ret = _fmp4->addTrack(track) ? true : ret;
    }
#endif

    //拷贝智能指针，目的是为了防止跨线程调用设置录像相关api导致的线程竞争问题
    auto hls = _hls;
    if (hls) {
        ret = hls->addTrack(track) ? true : ret;
    }
    auto mp4 = _mp4;
    if (mp4) {
        ret = mp4->addTrack(track) ? true : ret;
    }
    return ret;
}

void MultiMediaSourceMuxer::onAllTrackReady() {
    CHECK(!_create_in_poller || getOwnerPoller(MediaSource::NullMediaSource())->isCurrentThread());
    setMediaListener(getDelegate());

    if (_rtmp) {
        _rtmp->onAllTrackReady();
    }
    if (_rtsp) {
        _rtsp->onAllTrackReady();
    }
#if defined(ENABLE_MP4)
    if (_fmp4) {
        _fmp4->onAllTrackReady();
    }
#endif
    auto listener = _track_listener.lock();
    if (listener) {
        listener->onAllTrackReady();
    }

#if defined(ENABLE_RTPPROXY)
    GET_CONFIG(bool, gop_cache, RtpProxy::kGopCache);
    if (gop_cache) {
        createGopCacheIfNeed();
    }
#endif
    InfoL << "stream: " << shortUrl() << " , codec info: " << getTrackInfoStr(this);
}

void MultiMediaSourceMuxer::createGopCacheIfNeed() {
    if (_ring) {
        return;
    }
    weak_ptr<MultiMediaSourceMuxer> weak_self = shared_from_this();
    auto src = std::make_shared<MediaSourceForMuxer>(weak_self.lock());
    _ring = std::make_shared<RingType>(1024, [weak_self, src](int size) {
        if (auto strong_self = weak_self.lock()) {
            // 切换到归属线程
            strong_self->getOwnerPoller(MediaSource::NullMediaSource())->async([=]() {
                strong_self->onReaderChanged(*src, strong_self->totalReaderCount());
            });
        }
    });
}

void MultiMediaSourceMuxer::resetTracks() {
    MediaSink::resetTracks();

    if (_rtmp) {
        _rtmp->resetTracks();
    }
    if (_rtsp) {
        _rtsp->resetTracks();
    }
    if (_ts) {
        _ts->resetTracks();
    }
#if defined(ENABLE_MP4)
    if (_fmp4) {
        _fmp4->resetTracks();
    }
#endif

    //拷贝智能指针，目的是为了防止跨线程调用设置录像相关api导致的线程竞争问题
    auto hls = _hls;
    if (hls) {
        hls->resetTracks();
    }

    auto mp4 = _mp4;
    if (mp4) {
        mp4->resetTracks();
    }
}

bool MultiMediaSourceMuxer::onTrackFrame(const Frame::Ptr &frame_in) {
    auto frame = frame_in;
   if (_option.modify_stamp) {
        //开启了时间戳覆盖
        frame = std::make_shared<FrameStamp>(frame, _stamp[frame->getTrackType()],true);
    }

    bool ret = false;
    if (_rtmp) {
        ret = _rtmp->inputFrame(frame) ? true : ret;
    }
    if (_rtsp) {
        ret = _rtsp->inputFrame(frame) ? true : ret;
    }
    if (_ts) {
        ret = _ts->inputFrame(frame) ? true : ret;
    }

    //拷贝智能指针，目的是为了防止跨线程调用设置录像相关api导致的线程竞争问题
    //此处使用智能指针拷贝来确保线程安全，比互斥锁性能更优
    auto hls = _hls;
    if (hls) {
        ret = hls->inputFrame(frame) ? true : ret;
    }
    auto mp4 = _mp4;
    if (mp4) {
        ret = mp4->inputFrame(frame) ? true : ret;
    }

#if defined(ENABLE_MP4)
    if (_fmp4) {
        ret = _fmp4->inputFrame(frame) ? true : ret;
    }
#endif

    if (_ring) {
        if (frame->getTrackType() == TrackVideo) {
            // 视频时，遇到第一帧配置帧或关键帧则标记为gop开始处
            auto video_key_pos = frame->keyFrame() || frame->configFrame();
            _ring->write(frame, video_key_pos && !_video_key_pos);
            _video_key_pos = video_key_pos;
        } else {
            // 没有视频时，设置is_key为true，目的是关闭gop缓存
            _ring->write(frame, !haveVideo());
        }
    }
    return ret;
}

bool MultiMediaSourceMuxer::isEnabled(){
    GET_CONFIG(uint32_t, stream_none_reader_delay_ms, General::kStreamNoneReaderDelayMS);
    if (!_is_enable || _last_check.elapsedTime() > stream_none_reader_delay_ms) {
        //无人观看时，每次检查是否真的无人观看
        //有人观看时，则延迟一定时间检查一遍是否无人观看了(节省性能)
        auto hls = _hls;
        _is_enable = (_rtmp ? _rtmp->isEnabled() : false) ||
                     (_rtsp ? _rtsp->isEnabled() : false) ||
                     (_ts ? _ts->isEnabled() : false) ||
                     #if defined(ENABLE_MP4)
                     (_fmp4 ? _fmp4->isEnabled() : false) ||
                     #endif
                     (_ring ? (bool)_ring->readerCount() : false)  ||
                     (hls ? hls->isEnabled() : false) || _mp4;

        if (_is_enable) {
            //无人观看时，不刷新计时器,因为无人观看时每次都会检查一遍，所以刷新计数器无意义且浪费cpu
            _last_check.resetTime();
        }
    }
    return _is_enable;
}

}//namespace mediakit
