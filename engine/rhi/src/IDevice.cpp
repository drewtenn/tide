// Vtable anchor for the RHI virtual interfaces. Without this single TU emitting
// the destructors out-of-line, the vtable is duplicated in every TU that includes
// IDevice.h. Clang -Wweak-vtables flags that; some link configurations break.
// Herb Sutter, GotW #31.

#include "tide/rhi/IDevice.h"

namespace tide::rhi {

IDevice::~IDevice()                = default;
ICommandBuffer::~ICommandBuffer()  = default;
IFence::~IFence()                  = default;

} // namespace tide::rhi
