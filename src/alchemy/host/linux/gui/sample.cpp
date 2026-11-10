// Linux host backend, area "ui": sample_thread_stack (include/alchemy/host/ui.hpp), the hang
// diagnostic's view of another thread's host stack, through fx's thread sampling
// (fx/host/sample.hpp: a signal stops the thread, its registers and stack are copied, frame
// pointers are walked over the copy). Module names and offsets come from dladdr.
#include "alchemy/host/ui.hpp"

#include "fx/host/sample.hpp"

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <string>

namespace alchemy::host::ui {

Result<std::vector<StackFrame>> sample_thread_stack(std::uint64_t thread_id, std::size_t max_frames) {
    namespace sample = transmute::fx::host::sample;
    // A thread of this process is /proc/self/task/<tid>; one that has exited is gone from there.
    struct stat info {};
    if (thread_id == 0 || stat(("/proc/self/task/" + std::to_string(thread_id)).c_str(), &info) != 0) return Error::not_found;
    const auto thread = sample::open_thread(static_cast<std::uint32_t>(thread_id));
    if (!thread) return Error::not_found;
    // Everything the walk needs is allocated before the thread stops: it may hold the heap's lock.
    constexpr std::size_t stack_bytes = 64 * 1024;
    const auto copy = std::make_unique<std::uint8_t[]>(stack_bytes);
    std::vector<std::uint64_t> addresses(max_frames);
    unsigned depth = 0;
    if (max_frames && sample::suspend(thread)) {
        sample::Registers registers;
        if (sample::capture(thread, registers, true)) {
            const auto copied = sample::copy_stack(transmute::fx::host::rsp(registers.context()), copy.get(), stack_bytes);
            depth = sample::unwind_copy(registers, copy.get(), copied, addresses.data(),
                                        static_cast<unsigned>(std::min<std::size_t>(max_frames, 1u << 16)), nullptr);
        }
        sample::resume(thread);
    }
    std::vector<StackFrame> frames;
    for (unsigned index = 0; index < depth; ++index) {
        StackFrame frame{addresses[index]};
        Dl_info module{};
        if (dladdr(reinterpret_cast<void*>(frame.address), &module) && module.dli_fname) {
            std::string name = module.dli_fname;
            if (const auto slash = name.rfind('/'); slash != std::string::npos) name.erase(0, slash + 1);
            frame.module = name;
            frame.offset = frame.address - reinterpret_cast<std::uint64_t>(module.dli_fbase);
        }
        frames.push_back(std::move(frame));
    }
    return frames;
}

} // namespace alchemy::host::ui
