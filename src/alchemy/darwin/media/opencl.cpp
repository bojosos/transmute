#include "alchemy/darwin/media/media.hpp"

#include <array>
#include <string_view>

// OpenCL.framework: no OpenCL platform exists under Alchemy, so every entry point answers the error OpenCL
// itself defines for that case, and callers take their CPU paths (LibreOffice's clew wrapper links the framework
// and looks the functions up by name; darktable probes it). The values are those of the Khronos headers (cl.h,
// cl_ext.h; Apache-2.0).
// - clGetPlatformIDs: CL_PLATFORM_NOT_FOUND_KHR with *num_platforms 0 (CL_INVALID_VALUE for the arguments the
//   specification refuses).
// - Calls on a platform, device, context, command queue, memory object, sampler, program, kernel or event (none
//   can exist): that object's CL_INVALID_* code; clGetDeviceIDs also counts 0 devices.
// - Calls that make an object: NULL, with the code in errcode_ret.
// - clGetExtensionFunctionAddress: NULL; clUnloadCompiler: CL_SUCCESS (nothing is loaded).
namespace alchemy::gui::fill {
namespace {

constexpr std::string_view opencl = "/System/Library/Frameworks/OpenCL.framework/Versions/A/OpenCL";

// cl.h / cl_ext.h
constexpr std::int32_t cl_success = 0, cl_invalid_value = -30, cl_invalid_platform = -32, cl_invalid_device = -33, cl_invalid_context = -34,
    cl_invalid_command_queue = -36, cl_invalid_mem_object = -38, cl_invalid_sampler = -41, cl_invalid_program = -44, cl_invalid_kernel = -48,
    cl_invalid_event = -58, cl_platform_not_found_khr = -1001;

class OpenCL final : public Provider {
public:
    using Provider::Provider;
    void install();
};

void OpenCL::install() {
    // clGetPlatformIDs(num_entries, platforms, num_platforms)
    raw(opencl, "_clGetPlatformIDs", [](CpuState& s, GuestMemory& m) {
        if ((s.x[0] == 0 && s.x[1]) || (!s.x[1] && !s.x[2])) { s.x[0] = status(cl_invalid_value); return; }
        if (s.x[2]) m.write32(s.x[2], 0);
        s.x[0] = status(cl_platform_not_found_khr);
    });
    // clGetDeviceIDs(platform, device_type, num_entries, devices, num_devices)
    raw(opencl, "_clGetDeviceIDs", [](CpuState& s, GuestMemory& m) {
        if (s.x[4]) m.write32(s.x[4], 0);
        s.x[0] = status(cl_invalid_platform);
    });
    // The calls on an object, by the object's kind.
    const auto refuse = [this](std::int32_t code, std::initializer_list<const char*> names) {
        for (const auto* name : names) raw(opencl, std::string("_") + name, [code](CpuState& s, GuestMemory&) { s.x[0] = status(code); });
    };
    refuse(cl_invalid_platform, {"clGetPlatformInfo"});
    refuse(cl_invalid_device, {"clGetDeviceInfo"});
    refuse(cl_invalid_context, {"clGetContextInfo", "clRetainContext", "clReleaseContext", "clGetSupportedImageFormats"});
    refuse(cl_invalid_command_queue,
           {"clGetCommandQueueInfo", "clRetainCommandQueue", "clReleaseCommandQueue", "clSetCommandQueueProperty", "clFinish", "clFlush",
            "clEnqueueReadBuffer", "clEnqueueWriteBuffer", "clEnqueueCopyBuffer", "clEnqueueReadImage", "clEnqueueWriteImage", "clEnqueueCopyImage",
            "clEnqueueCopyImageToBuffer", "clEnqueueCopyBufferToImage", "clEnqueueUnmapMemObject", "clEnqueueNDRangeKernel", "clEnqueueTask",
            "clEnqueueNativeKernel", "clEnqueueMarker", "clEnqueueWaitForEvents", "clEnqueueBarrier"});
    refuse(cl_invalid_mem_object, {"clGetMemObjectInfo", "clGetImageInfo", "clRetainMemObject", "clReleaseMemObject"});
    refuse(cl_invalid_sampler, {"clGetSamplerInfo", "clRetainSampler", "clReleaseSampler"});
    refuse(cl_invalid_program, {"clBuildProgram", "clGetProgramInfo", "clGetProgramBuildInfo", "clRetainProgram", "clReleaseProgram",
                                "clCreateKernelsInProgram"});
    refuse(cl_invalid_kernel, {"clSetKernelArg", "clGetKernelInfo", "clGetKernelWorkGroupInfo", "clRetainKernel", "clReleaseKernel"});
    refuse(cl_invalid_event, {"clGetEventInfo", "clGetEventProfilingInfo", "clRetainEvent", "clReleaseEvent"});
    // clWaitForEvents(num_events, event_list)
    raw(opencl, "_clWaitForEvents", [](CpuState& s, GuestMemory&) { s.x[0] = status(s.x[0] && s.x[1] ? cl_invalid_event : cl_invalid_value); });
    raw(opencl, "_clUnloadCompiler", [](CpuState& s, GuestMemory&) { s.x[0] = status(cl_success); });
    raw(opencl, "_clGetExtensionFunctionAddress", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    // The calls that make an object: NULL, the code in errcode_ret (the register or stack slot each signature puts it in).
    const auto make = [this](const char* name, std::int32_t code, int errcode_register, int errcode_stack_slot = -1) {
        raw(opencl, std::string("_") + name, [code, errcode_register, errcode_stack_slot](CpuState& s, GuestMemory& m) {
            const auto errcode = errcode_register >= 0 ? s.x[static_cast<std::size_t>(errcode_register)] : m.read64(s.sp + 8 * static_cast<std::uint64_t>(errcode_stack_slot));
            if (errcode) m.write32(errcode, static_cast<std::uint32_t>(code));
            s.x[0] = 0;
        });
    };
    // clCreateContext(properties, num_devices, devices, pfn_notify, user_data, errcode_ret): no device exists.
    raw(opencl, "_clCreateContext", [](CpuState& s, GuestMemory& m) {
        if (s.x[5]) m.write32(s.x[5], static_cast<std::uint32_t>(s.x[1] && s.x[2] ? cl_invalid_device : cl_invalid_value));
        s.x[0] = 0;
    });
    make("clCreateContextFromType", cl_invalid_platform, 4);   // (properties, device_type, pfn_notify, user_data, errcode_ret)
    make("clCreateCommandQueue", cl_invalid_context, 3);       // (context, device, properties, errcode_ret)
    make("clCreateBuffer", cl_invalid_context, 4);             // (context, flags, size, host_ptr, errcode_ret)
    make("clCreateImage2D", cl_invalid_context, 7);            // (context, flags, format, width, height, row_pitch, host_ptr, errcode_ret)
    make("clCreateImage3D", cl_invalid_context, -1, 1);        // (..., row_pitch, slice_pitch, [stack] host_ptr, errcode_ret)
    make("clCreateSampler", cl_invalid_context, 4);            // (context, normalized, addressing, filter, errcode_ret)
    make("clCreateProgramWithSource", cl_invalid_context, 4);  // (context, count, strings, lengths, errcode_ret)
    make("clCreateProgramWithBinary", cl_invalid_context, 6);  // (context, num_devices, devices, lengths, binaries, status, errcode_ret)
    make("clCreateKernel", cl_invalid_program, 2);             // (program, kernel_name, errcode_ret)
    // (queue, buffer, blocking, flags, offset, size, num_events, wait_list, [stack] event, errcode_ret)
    make("clEnqueueMapBuffer", cl_invalid_command_queue, -1, 1);
    // (queue, image, blocking, flags, origin, region, row_pitch, slice_pitch, [stack] num_events, wait_list, event, errcode_ret)
    make("clEnqueueMapImage", cl_invalid_command_queue, -1, 3);
}

} // namespace

void install_opencl(InstallContext& context) { std::make_shared<OpenCL>(context)->install(); }

} // namespace alchemy::gui::fill
