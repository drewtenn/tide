// CaptureManager — wraps MTLCaptureManager. Locked DEFINE D12: queue-scoped
// capture, MTL_CAPTURE_ENABLED=1 env var prereq for non-Xcode launches.

#import "MetalDevice.h"
#import "tide/rhi-metal/MetalDevice.h"

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include <cstdio>

namespace tide::rhi::metal {

bool begin_frame_capture(tide::rhi::IDevice& device, std::string_view label) {
    @autoreleasepool {
        auto& md = static_cast<MetalDevice&>(device);
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)md.mtl_command_queue();
        if (!queue) return false;

        MTLCaptureManager* mgr = [MTLCaptureManager sharedCaptureManager];
        if (![mgr supportsDestination:MTLCaptureDestinationGPUTraceDocument]) {
            return false;
        }

        MTLCaptureDescriptor* desc = [[MTLCaptureDescriptor alloc] init];
        desc.captureObject = queue;
        desc.destination   = MTLCaptureDestinationGPUTraceDocument;

        NSDateFormatter* fmt = [[NSDateFormatter alloc] init];
        fmt.dateFormat = @"yyyyMMdd_HHmmss";
        NSString* ts = [fmt stringFromDate:[NSDate date]];

        NSString* nslabel = label.empty()
            ? @"capture"
            : [[NSString alloc] initWithBytes:label.data()
                                       length:label.size()
                                     encoding:NSUTF8StringEncoding];
        NSString* filename = [NSString stringWithFormat:@"tide_%@_%@.gputrace", ts, nslabel];

        NSString* dir = [[NSProcessInfo processInfo].environment objectForKey:@"TIDE_CAPTURE_DIR"];
        if (!dir) dir = NSTemporaryDirectory();
        NSString* path = [dir stringByAppendingPathComponent:filename];
        desc.outputURL = [NSURL fileURLWithPath:path];

        NSError* err = nil;
        if (![mgr startCaptureWithDescriptor:desc error:&err]) {
            std::fprintf(stderr, "tide.capture: startCapture failed: %s\n",
                         err ? err.localizedDescription.UTF8String : "unknown");
            return false;
        }
        return true;
    }
}

void end_frame_capture() {
    [[MTLCaptureManager sharedCaptureManager] stopCapture];
}

} // namespace tide::rhi::metal
