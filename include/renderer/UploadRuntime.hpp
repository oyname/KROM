#pragma once

#include "renderer/RendererTypes.hpp"
#include <cstdint>

namespace engine::renderer {

enum class UploadPathTopology : uint8_t
{
    StageThenCopy = 0,
};

enum class UploadSubmissionModel : uint8_t
{
    SingleGraphicsQueueV1 = 0,
    MultiQueueDeferred,
};

enum class UploadStagingLifetimeModel : uint8_t
{
    RetirePerSubmissionFence = 0,
};

enum class UploadTransitionPolicy : uint8_t
{
    ExplicitBeforeAndAfterCopy = 0,
};

struct UploadRuntimeDesc
{
    UploadPathTopology topology = UploadPathTopology::StageThenCopy;
    UploadSubmissionModel submissionModel = UploadSubmissionModel::SingleGraphicsQueueV1;
    UploadStagingLifetimeModel stagingLifetime = UploadStagingLifetimeModel::RetirePerSubmissionFence;
    UploadTransitionPolicy transitionPolicy = UploadTransitionPolicy::ExplicitBeforeAndAfterCopy;
    QueueType recordingQueue = QueueType::Graphics;
    bool cpuWritesGoToStaging = true;
    bool copiesRecordedExplicitly = true;
    bool destinationStatePreservedAcrossUpload = true;
    bool destinationStateAuthorityIsBackendResource = true;
    bool stagingReleaseDeferredByFence = true;
    bool uploadCommandContextTransient = true;
};

[[nodiscard]] inline UploadRuntimeDesc BuildDefaultUploadRuntimeDesc() noexcept
{
    return UploadRuntimeDesc{};
}

} // namespace engine::renderer
