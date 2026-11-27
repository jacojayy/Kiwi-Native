// PNRT shim for Eden's video_core/gpu.h: the lifted code asks the GPU for DSModAux() and (runtime
// 24) for guest texture readback, which this host cannot do (no texture cache): the capability is
// not advertised and a request is refused.
#pragma once

#include "video_core/dsmod/aux_routing.h"
#include "video_core/dsmod/texture_readback.h"
#include "pnrt_services.h"

namespace Tegra {

class GPU {
public:
    [[nodiscard]] VideoCore::DSMod::AuxRouting& DSModAux() {
        return aux;
    }
    [[nodiscard]] bool DSModTextureReadbackAvailable() const {
        return PnShim::ReadbackAvailable();
    }
    bool DSModRequestTextureReadback(const VideoCore::DSMod::ReadbackRequest& request,
                                     VideoCore::DSMod::ReadbackCallback callback) {
        return PnShim::ReadbackSubmit(request, std::move(callback));
    }
    VideoCore::DSMod::AuxRouting aux;
};

} // namespace Tegra
