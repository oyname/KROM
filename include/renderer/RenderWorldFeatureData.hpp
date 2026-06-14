#pragma once

// Compatibility forwarding header.
// The FeatureData/View contract has exactly one owning definition in
// RenderFeatureDataViews.hpp. Do not define FeatureDataStorageBase,
// FeatureDataStorage<T>, RenderFeatureDataSlot or RenderFeatureDataRegistry here.
#include "renderer/RenderFeatureDataViews.hpp"
