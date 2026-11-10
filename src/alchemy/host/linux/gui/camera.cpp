// Linux host backend, area "camera" (include/alchemy/host/camera.hpp), over Video4Linux2. Cameras are the
// /dev/video* nodes whose device capabilities include video capture, by path, with the driver's card name. Their
// formats are the native ones this backend delivers as NV12: NV12 itself and packed YUYV ("YUY2", converted).
// Compressed formats (MJPG, H.264) would need a decoder and are not listed. Each size's fastest and slowest frame
// rates come from the driver's frame intervals (stepwise and continuous ranges give their largest size).
// Listing opens each node briefly. No /dev/video node (WSL, a container): no cameras.
// A stream sets the chosen format and rate, maps four driver buffers, and while running one thread waits on the
// device (and an eventfd that stops it), converts each dequeued frame to tightly packed NV12 and delivers it with
// the host time. Linux desktops keep no camera privacy setting a program can read, so the access is unknown; a
// node the user may not open answers Error::access_denied.
#include "alchemy/host/camera.hpp"
#include "alchemy/host/time.hpp"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <thread>

namespace alchemy::host::camera {

namespace {

int xioctl(int fd, unsigned long request, void* argument) {
    int result;
    do result = ::ioctl(fd, request, argument);
    while (result < 0 && errno == EINTR);
    return result;
}

std::uint32_t fourcc(const char (&code)[5]) {
    return std::uint32_t(std::uint8_t(code[0])) | (std::uint32_t(std::uint8_t(code[1])) << 8) | (std::uint32_t(std::uint8_t(code[2])) << 16) |
           (std::uint32_t(std::uint8_t(code[3])) << 24);
}

// The subtype a camera format reports for a V4L2 pixel format this backend converts, or 0.
std::uint32_t subtype_of(std::uint32_t pixel_format) {
    if (pixel_format == V4L2_PIX_FMT_NV12) return fourcc("NV12");
    if (pixel_format == V4L2_PIX_FMT_YUYV) return fourcc("YUY2");
    return 0;
}

struct Native {
    Format format;
    std::uint32_t pixel_format{};
};

// The rates of one size: {slowest, fastest} frames per second (0, 0 when the driver gives none).
std::pair<double, double> rates(int fd, std::uint32_t pixel_format, std::uint32_t width, std::uint32_t height) {
    v4l2_frmivalenum interval{};
    interval.pixel_format = pixel_format;
    interval.width = width;
    interval.height = height;
    double slowest = 0, fastest = 0;
    const auto rate_of = [](const v4l2_fract& f) { return f.numerator ? double(f.denominator) / f.numerator : 0.0; };
    for (interval.index = 0; xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) == 0; ++interval.index) {
        if (interval.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            const double r = rate_of(interval.discrete);
            if (r <= 0) continue;
            fastest = std::max(fastest, r);
            slowest = slowest == 0 ? r : std::min(slowest, r);
        } else {
            fastest = rate_of(interval.stepwise.min);
            slowest = rate_of(interval.stepwise.max);
            break;
        }
    }
    return {slowest, fastest};
}

// The formats of an open node this backend delivers, in the driver's order.
std::vector<Native> natives(int fd) {
    std::vector<Native> out;
    v4l2_fmtdesc description{};
    description.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (description.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &description) == 0; ++description.index) {
        const auto subtype = subtype_of(description.pixelformat);
        if (!subtype) continue;
        v4l2_frmsizeenum size{};
        size.pixel_format = description.pixelformat;
        for (size.index = 0; xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) == 0; ++size.index) {
            std::uint32_t width, height;
            if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                width = size.discrete.width;
                height = size.discrete.height;
            } else {
                width = size.stepwise.max_width;
                height = size.stepwise.max_height;
            }
            if ((width & 1) || (height & 1) || width == 0 || height == 0) continue;   // NV12 needs whole chroma samples
            const auto [slowest, fastest] = rates(fd, description.pixelformat, width, height);
            out.push_back({{width, height, slowest, fastest, subtype}, description.pixelformat});
            if (size.type != V4L2_FRMSIZE_TYPE_DISCRETE) break;
        }
    }
    return out;
}

// Opens a capture node: its descriptor, or -1 with errno.
int open_capture(const std::string& path, std::string* card) {
    const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    v4l2_capability capability{};
    if (xioctl(fd, VIDIOC_QUERYCAP, &capability) < 0) {
        ::close(fd);
        errno = ENODEV;
        return -1;
    }
    const auto caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) ? capability.device_caps : capability.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
        ::close(fd);
        errno = ENODEV;
        return -1;
    }
    if (card) *card = reinterpret_cast<const char*>(capability.card);
    return fd;
}

std::vector<std::string> nodes() {
    std::vector<std::string> out;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/dev", error)) {
        const auto name = entry.path().filename().string();
        if (name.rfind("video", 0) == 0 && name.size() > 5 && std::all_of(name.begin() + 5, name.end(), [](char c) { return c >= '0' && c <= '9'; }))
            out.push_back(entry.path().string());
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    return out;
}

} // namespace

Result<std::vector<Camera>> cameras() {
    std::vector<Camera> out;
    for (const auto& path : nodes()) {
        std::string card;
        const int fd = open_capture(path, &card);
        if (fd < 0) continue;
        Camera c;
        c.id = path;
        c.name = card;
        for (const auto& n : natives(fd)) c.formats.push_back(n.format);
        ::close(fd);
        if (!c.formats.empty()) out.push_back(std::move(c));
    }
    return out;
}

Access camera_access() { return Access::unknown; }

class Stream {
public:
    int fd{-1};
    int wake{-1};
    std::uint32_t width{}, height{}, pixel_format{}, bytes_per_line{};
    struct Buffer {
        void* data{};
        std::size_t length{};
    };
    std::vector<Buffer> buffers;
    Deliver deliver;
    std::thread reader;
    std::vector<std::uint8_t> nv12;

    ~Stream() {
        for (const auto& b : buffers)
            if (b.data) ::munmap(b.data, b.length);
        if (wake >= 0) ::close(wake);
        if (fd >= 0) ::close(fd);
    }

    // The frame in buffer `data` as tightly packed NV12 in `nv12`.
    void convert(const std::uint8_t* data, std::size_t used) {
        const std::size_t luma = std::size_t(width) * height;
        nv12.resize(luma + luma / 2);
        if (pixel_format == V4L2_PIX_FMT_NV12) {
            const std::size_t pitch = bytes_per_line ? bytes_per_line : width;
            if (used < pitch * height + pitch * (height / 2)) return;
            for (std::uint32_t y = 0; y < height; ++y) std::memcpy(nv12.data() + std::size_t(y) * width, data + y * pitch, width);
            const auto* chroma = data + pitch * height;
            for (std::uint32_t y = 0; y < height / 2; ++y) std::memcpy(nv12.data() + luma + std::size_t(y) * width, chroma + y * pitch, width);
            return;
        }
        // YUYV: Y0 U Y1 V per two pixels; the chroma of two rows averaged.
        const std::size_t pitch = bytes_per_line ? bytes_per_line : std::size_t(width) * 2;
        if (used < pitch * height) return;
        for (std::uint32_t y = 0; y < height; ++y) {
            const auto* row = data + y * pitch;
            for (std::uint32_t x = 0; x < width; ++x) nv12[std::size_t(y) * width + x] = row[2 * x];
        }
        for (std::uint32_t y = 0; y < height / 2; ++y) {
            const auto* top = data + (2 * y) * pitch;
            const auto* bottom = top + pitch;
            auto* out = nv12.data() + luma + std::size_t(y) * width;
            for (std::uint32_t x = 0; x < width / 2; ++x) {
                out[2 * x] = static_cast<std::uint8_t>((top[4 * x + 1] + bottom[4 * x + 1] + 1) / 2);
                out[2 * x + 1] = static_cast<std::uint8_t>((top[4 * x + 3] + bottom[4 * x + 3] + 1) / 2);
            }
        }
    }

    void read_frames() {
        pollfd fds[2] = {{wake, POLLIN, 0}, {fd, POLLIN, 0}};
        for (;;) {
            if (::poll(fds, 2, -1) < 0) {
                if (errno == EINTR) continue;
                return;
            }
            if (fds[0].revents & POLLIN) return;
            if (fds[1].revents & (POLLERR | POLLHUP)) return;   // unplugged
            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            if (xioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
                if (errno == EAGAIN) continue;
                return;
            }
            const auto now = time::monotonic_ns();
            if (buffer.index < buffers.size() && !(buffer.flags & V4L2_BUF_FLAG_ERROR)) {
                nv12.clear();
                convert(static_cast<const std::uint8_t*>(buffers[buffer.index].data), buffer.bytesused);
                if (!nv12.empty()) deliver({width, height, now, nv12.data(), nv12.size()});
            }
            if (xioctl(fd, VIDIOC_QBUF, &buffer) < 0) return;
        }
    }
};

Result<Stream*> open(std::string_view id, std::uint32_t width, std::uint32_t height, double rate, Deliver deliver) {
    if (!deliver) return Error::invalid_argument;
    const auto listed = nodes();
    if (std::find(listed.begin(), listed.end(), std::string(id)) == listed.end()) return listed.empty() ? Error::unsupported : Error::not_found;
    auto stream = std::make_unique<Stream>();
    stream->fd = open_capture(std::string(id), nullptr);
    if (stream->fd < 0) return errno == EACCES || errno == EPERM ? Error::access_denied : Error::not_found;
    const auto formats = natives(stream->fd);
    if (formats.empty()) return Error::unsupported;   // nothing this backend delivers
    // The size closest to the one asked for (the first without one); NV12 before YUYV at that size.
    const Native* chosen = &formats.front();
    if (width && height) {
        double best = std::numeric_limits<double>::max();
        for (const auto& n : formats) {
            const double d = std::abs(double(n.format.width) - width) + std::abs(double(n.format.height) - height) +
                             (n.pixel_format == V4L2_PIX_FMT_NV12 ? 0.0 : 0.5);
            if (d < best) { best = d; chosen = &n; }
        }
    }
    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = chosen->format.width;
    format.fmt.pix.height = chosen->format.height;
    format.fmt.pix.pixelformat = chosen->pixel_format;
    format.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(stream->fd, VIDIOC_S_FMT, &format) < 0 || !subtype_of(format.fmt.pix.pixelformat) || (format.fmt.pix.width & 1) ||
        (format.fmt.pix.height & 1))
        return Error::io;
    stream->width = format.fmt.pix.width;
    stream->height = format.fmt.pix.height;
    stream->pixel_format = format.fmt.pix.pixelformat;
    stream->bytes_per_line = format.fmt.pix.bytesperline;
    // At most `rate` frames per second (the driver's fastest without one).
    const double wanted = rate > 0 ? std::min(rate, chosen->format.max_rate > 0 ? chosen->format.max_rate : rate) : chosen->format.max_rate;
    if (wanted > 0) {
        v4l2_streamparm parameters{};
        parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(stream->fd, VIDIOC_G_PARM, &parameters) == 0 && (parameters.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
            parameters.parm.capture.timeperframe = {1000, static_cast<std::uint32_t>(std::lround(wanted * 1000))};
            static_cast<void>(xioctl(stream->fd, VIDIOC_S_PARM, &parameters));
        }
    }
    v4l2_requestbuffers request{};
    request.count = 4;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (xioctl(stream->fd, VIDIOC_REQBUFS, &request) < 0 || request.count == 0) return Error::io;
    for (std::uint32_t i = 0; i < request.count; ++i) {
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        if (xioctl(stream->fd, VIDIOC_QUERYBUF, &buffer) < 0) return Error::io;
        void* data = ::mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, stream->fd, buffer.m.offset);
        if (data == MAP_FAILED) return Error::no_memory;
        stream->buffers.push_back({data, buffer.length});
    }
    stream->deliver = std::move(deliver);
    return stream.release();
}

void size(const Stream* stream, std::uint32_t& width, std::uint32_t& height) noexcept {
    width = stream ? stream->width : 0;
    height = stream ? stream->height : 0;
}

Status start(Stream* stream) {
    if (!stream) return Error::invalid_argument;
    if (stream->reader.joinable()) return Error::none;
    for (std::uint32_t i = 0; i < stream->buffers.size(); ++i) {
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        if (xioctl(stream->fd, VIDIOC_QBUF, &buffer) < 0) return Error::io;
    }
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(stream->fd, VIDIOC_STREAMON, &type) < 0) return Error::io;
    if (stream->wake < 0) stream->wake = ::eventfd(0, EFD_CLOEXEC);
    if (stream->wake < 0) return Error::io;
    stream->reader = std::thread([stream] { stream->read_frames(); });
    return Error::none;
}

void stop(Stream* stream) noexcept {
    if (!stream || !stream->reader.joinable()) return;
    const std::uint64_t one = 1;
    static_cast<void>(::write(stream->wake, &one, sizeof one));
    stream->reader.join();
    std::uint64_t drained = 0;
    static_cast<void>(::read(stream->wake, &drained, sizeof drained));
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    static_cast<void>(xioctl(stream->fd, VIDIOC_STREAMOFF, &type));   // returns every buffer to the application
}

void close(Stream* stream) noexcept {
    if (!stream) return;
    stop(stream);
    delete stream;
}

} // namespace alchemy::host::camera
