#pragma once

#include <memory>
#include <string>
#include "mirror_types.h"

class MirrorReceiver {
public:
    // 端口固定为 27183，不再需要 directHost
    explicit MirrorReceiver(SharedState& state, int port = PORT);
    ~MirrorReceiver();

    MirrorReceiver(const MirrorReceiver&) = delete;
    MirrorReceiver& operator=(const MirrorReceiver&) = delete;

    void start();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};