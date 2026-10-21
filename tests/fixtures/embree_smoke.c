/* Minimal ARM64 guest that exercises the packaged Embree DLL directly. */
typedef void* RTCDevice;
typedef void* RTCScene;
typedef void* RTCGeometry;

__declspec(dllimport) RTCDevice rtcNewDevice(const char* config);
__declspec(dllimport) unsigned int rtcGetDeviceError(RTCDevice device);
#if defined(EMBREE_EXTENDED_APIS)
enum ProbeRTCError { PROBE_RTC_ERROR_NONE = 0 };
typedef void (*ProbeErrorFunction)(void*, enum ProbeRTCError, const char*);
typedef _Bool (*ProbeMemoryMonitorFunction)(void*, long long, _Bool);
__declspec(dllimport) void rtcSetDeviceErrorFunction(RTCDevice device,
    ProbeErrorFunction callback, void* user);
__declspec(dllimport) void rtcSetDeviceMemoryMonitorFunction(RTCDevice device,
    ProbeMemoryMonitorFunction callback, void* user);
#endif
__declspec(dllimport) RTCScene rtcNewScene(RTCDevice device);
__declspec(dllimport) void rtcSetSceneFlags(RTCScene scene, unsigned int flags);
__declspec(dllimport) void rtcSetSceneBuildQuality(RTCScene scene,
    unsigned int quality);
__declspec(dllimport) void rtcSetSceneProgressMonitorFunction(RTCScene scene,
    _Bool (*callback)(void*, double), void* user);
typedef __declspec(align(16)) struct {
    float lower_x, lower_y, lower_z, align0;
    float upper_x, upper_y, upper_z, align1;
} ProbeBounds;
__declspec(dllimport) void rtcGetSceneBounds(RTCScene scene, ProbeBounds* bounds);
__declspec(dllimport) RTCGeometry rtcNewGeometry(RTCDevice device, unsigned int type);
__declspec(dllimport) void* rtcSetNewGeometryBuffer(RTCGeometry geometry,
    unsigned int type, unsigned int slot, unsigned int format,
    unsigned long long stride, unsigned long long count);
__declspec(dllimport) void rtcSetNewGeometryBufferHostDevice(RTCGeometry geometry,
    unsigned int type, unsigned int slot, unsigned int format,
    unsigned long long stride, unsigned long long count,
    void** host_pointer, void** device_pointer);
__declspec(dllimport) void rtcSetGeometryUserData(RTCGeometry geometry, void* data);
__declspec(dllimport) void rtcSetGeometryMask(RTCGeometry geometry, unsigned int mask);
__declspec(dllimport) void rtcSetGeometryBuildQuality(RTCGeometry geometry,
    unsigned int quality);
__declspec(dllimport) void rtcSetGeometryEnableFilterFunctionFromArguments(
    RTCGeometry geometry, int enabled);
__declspec(dllimport) void rtcSetGeometryInstancedScene(RTCGeometry geometry,
    RTCScene scene);
__declspec(dllimport) void rtcSetGeometryTimeStepCount(RTCGeometry geometry,
    unsigned int count);
__declspec(dllimport) void rtcSetGeometryTransform(RTCGeometry geometry,
    unsigned int time_step, unsigned int format, const void* transform);
#if defined(EMBREE_EXTENDED_APIS)
typedef __declspec(align(16)) struct {
    float scale_x, scale_y, scale_z;
    float skew_xy, skew_xz, skew_yz;
    float shift_x, shift_y, shift_z;
    float quaternion_r, quaternion_i, quaternion_j, quaternion_k;
    float translation_x, translation_y, translation_z;
} ProbeQuaternionDecomposition;
_Static_assert(sizeof(ProbeQuaternionDecomposition) == 64,
               "Embree quaternion decomposition layout changed");
__declspec(dllimport) void rtcSetGeometryTransformQuaternion(
    RTCGeometry geometry, unsigned int time_step,
    const ProbeQuaternionDecomposition* decomposition);
__declspec(dllimport) void rtcSetGeometryTessellationRate(
    RTCGeometry geometry, float rate);
__declspec(dllimport) void rtcSetSharedGeometryBuffer(RTCGeometry geometry,
    unsigned int type, unsigned int slot, unsigned int format,
    const void* pointer, unsigned long long offset, unsigned long long stride,
    unsigned long long count);
#endif
__declspec(dllimport) void rtcUpdateGeometryBuffer(RTCGeometry geometry,
    unsigned int type, unsigned int slot);
__declspec(dllimport) void rtcCommitGeometry(RTCGeometry geometry);
__declspec(dllimport) unsigned int rtcAttachGeometry(RTCScene scene,
    RTCGeometry geometry);
__declspec(dllimport) void rtcAttachGeometryByID(RTCScene scene,
    RTCGeometry geometry, unsigned int geometry_id);
__declspec(dllimport) RTCGeometry rtcGetGeometry(RTCScene scene,
    unsigned int geometry_id);
__declspec(dllimport) void* rtcGetGeometryBufferData(RTCGeometry geometry,
    unsigned int type, unsigned int slot);
__declspec(dllimport) void* rtcGetGeometryUserDataFromTraversable(
    void* traversable, unsigned int geometry_id);
__declspec(dllimport) void rtcReleaseGeometry(RTCGeometry geometry);
__declspec(dllimport) void rtcCommitScene(RTCScene scene);
__declspec(dllimport) void rtcReleaseScene(RTCScene scene);
__declspec(dllimport) void rtcReleaseDevice(RTCDevice device);

/* Keep these fields in the order and alignment of Embree 4's public RTCRayHit. */
typedef __declspec(align(16)) struct {
    float org_x, org_y, org_z, tnear;
    float dir_x, dir_y, dir_z, time;
    float tfar;
    unsigned int mask, id, flags;
} ProbeRay;
typedef __declspec(align(16)) struct {
    float Ng_x, Ng_y, Ng_z, u, v;
    unsigned int primID, geomID;
    unsigned int instID[16];
    /* Embree builds with instance arrays append this optional field. */
    unsigned int instPrimID[16];
} ProbeHit;
typedef struct { ProbeRay ray; ProbeHit hit; } ProbeRayHit;
_Static_assert(sizeof(ProbeRay) == 48, "single ray layout changed");
_Static_assert(__builtin_offsetof(ProbeRayHit, hit) == 48,
               "single hit offset changed");
__declspec(dllimport) void rtcIntersect1(RTCScene scene, ProbeRayHit* rayhit,
                                         void* arguments);
__declspec(dllimport) void rtcOccluded1(RTCScene scene, ProbeRay* ray,
                                        void* arguments);
__declspec(dllimport) void* rtcGetSceneTraversable(RTCScene scene);
__declspec(dllimport) void rtcTraversableIntersect1(void* traversable,
    ProbeRayHit* rayhit, void* arguments);
__declspec(dllimport) void rtcTraversableOccluded1(void* traversable,
    ProbeRay* ray, void* arguments);
#if defined(EMBREE_ARGUMENT_FILTER)
typedef struct {
    int* valid;
    void* geometry_user;
    void* context;
    void* ray;
    void* hit;
    unsigned int count;
} ProbeFilterArguments;
typedef void (*ProbeFilterFunction)(const ProbeFilterArguments*);
typedef struct {
    unsigned int flags, feature_mask;
    void* context;
    ProbeFilterFunction filter;
    void* intersection;
    float min_width_distance_factor;
} ProbeIntersectArguments;
typedef ProbeIntersectArguments ProbeOccludedArguments;
_Static_assert(__builtin_offsetof(ProbeIntersectArguments, filter) == 16,
               "Embree ray-query filter offset changed");
static _Atomic unsigned int filter_calls;
static _Atomic unsigned int filter_errors;
static void reject_filter(const ProbeFilterArguments* args) {
    if (!args || args->count != 1 || !args->valid ||
        args->valid[0] != -1 || args->geometry_user != (void*)0x12345678)
        __c11_atomic_fetch_add(&filter_errors, 1, __ATOMIC_RELAXED);
    else
        args->valid[0] = 0;
    __c11_atomic_fetch_add(&filter_calls, 1, __ATOMIC_RELAXED);
}
#endif
typedef __declspec(align(16)) struct {
    float org_x[4], org_y[4], org_z[4], tnear[4];
    float dir_x[4], dir_y[4], dir_z[4], time[4], tfar[4];
    unsigned int mask[4], id[4], flags[4];
} ProbeRay4;
typedef __declspec(align(16)) struct {
    float Ng_x[4], Ng_y[4], Ng_z[4], u[4], v[4];
    unsigned int primID[4], geomID[4], instID[16][4], instPrimID[16][4];
} ProbeHit4;
typedef struct { ProbeRay4 ray; ProbeHit4 hit; } ProbeRayHit4;
_Static_assert(__builtin_offsetof(ProbeRayHit4, hit) == 192,
               "four-ray hit offset changed");
__declspec(dllimport) void rtcIntersect4(const int* valid, RTCScene scene,
                                         ProbeRayHit4* rayhit, void* arguments);
__declspec(dllimport) void rtcOccluded4(const int* valid, RTCScene scene,
                                        ProbeRay4* ray, void* arguments);

#define DECLARE_PACKET(N, ALIGNMENT) \
typedef __declspec(align(ALIGNMENT)) struct { \
    float org_x[N], org_y[N], org_z[N], tnear[N]; \
    float dir_x[N], dir_y[N], dir_z[N], time[N], tfar[N]; \
    unsigned int mask[N], id[N], flags[N]; \
} ProbeRay##N; \
typedef __declspec(align(ALIGNMENT)) struct { \
    float Ng_x[N], Ng_y[N], Ng_z[N], u[N], v[N]; \
    unsigned int primID[N], geomID[N], instID[16][N], instPrimID[16][N]; \
} ProbeHit##N; \
typedef struct { ProbeRay##N ray; ProbeHit##N hit; } ProbeRayHit##N; \
__declspec(dllimport) void rtcIntersect##N(const int* valid, RTCScene scene, \
                                            ProbeRayHit##N* rayhit, void* arguments); \
__declspec(dllimport) void rtcOccluded##N(const int* valid, RTCScene scene, \
                                           ProbeRay##N* ray, void* arguments)

DECLARE_PACKET(8, 32);
DECLARE_PACKET(16, 64);
_Static_assert(__builtin_offsetof(ProbeRayHit8, hit) == 384,
               "eight-ray hit offset changed");
_Static_assert(__builtin_offsetof(ProbeRayHit16, hit) == 768,
               "sixteen-ray hit offset changed");

#define CHECK_PACKET(N, HIT_ERROR, MISS_ERROR, OCCLUDED_ERROR, CLEAR_ERROR) do { \
    static ProbeRayHit##N packet; \
    int valid[N]; \
    for (unsigned int lane = 0; lane < N; ++lane) { \
        valid[lane] = -1; \
        packet.ray.org_x[lane] = (lane & 1) ? 2.0f : 0.25f; \
        packet.ray.org_y[lane] = (lane & 1) ? 2.0f : 0.25f; \
        packet.ray.org_z[lane] = -1.0f; \
        packet.ray.dir_z[lane] = 1.0f; \
        packet.ray.tfar[lane] = 100.0f; \
        packet.ray.mask[lane] = ~0u; \
        packet.hit.geomID[lane] = ~0u; \
    } \
    rtcIntersect##N(valid, scene, &packet, 0); \
    for (unsigned int lane = 0; lane < N; ++lane) { \
        if ((lane & 1) == 0) { \
            if (packet.hit.geomID[lane] != geometry_id || \
                packet.hit.primID[lane] >= triangle_count || \
                packet.ray.tfar[lane] < 0.99f || \
                packet.ray.tfar[lane] > 1.01f) return HIT_ERROR; \
        } else if (packet.hit.geomID[lane] != ~0u || \
                   packet.ray.tfar[lane] != 100.0f) return MISS_ERROR; \
        packet.ray.tfar[lane] = 100.0f; \
    } \
    rtcOccluded##N(valid, scene, &packet.ray, 0); \
    for (unsigned int lane = 0; lane < N; ++lane) { \
        if ((lane & 1) == 0) { \
            if (packet.ray.tfar[lane] >= 0.0f) return OCCLUDED_ERROR; \
        } else if (packet.ray.tfar[lane] != 100.0f) return CLEAR_ERROR; \
    } \
} while (0)

#if defined(EMBREE_CALLBACKS)
static _Atomic unsigned int progress_calls;
static _Bool progress_callback(void* user, double fraction) {
    if (user != &progress_calls || fraction < 0.0 || fraction > 1.0) return 0;
    __c11_atomic_fetch_add(&progress_calls, 1, __ATOMIC_RELAXED);
    return 1;
}
#endif

#if defined(EMBREE_EXTENDED_APIS)
static _Atomic unsigned int device_error_calls;
static _Atomic unsigned int memory_monitor_calls;
static _Atomic unsigned int monitored_allocation_requests;
static _Atomic unsigned int callback_context_errors;
static unsigned int callback_context;
static void device_error_callback(void* user, enum ProbeRTCError error,
                                  const char* message) {
    if (user != &callback_context || error == PROBE_RTC_ERROR_NONE ||
        message == 0)
        __c11_atomic_fetch_add(&callback_context_errors, 1, __ATOMIC_RELAXED);
    __c11_atomic_fetch_add(&device_error_calls, 1, __ATOMIC_RELAXED);
}
static _Bool device_memory_callback(void* user, long long bytes, _Bool post) {
    (void)post;
    __c11_atomic_fetch_add(&memory_monitor_calls, 1, __ATOMIC_RELAXED);
    if (user != &callback_context)
        __c11_atomic_fetch_add(&callback_context_errors, 1, __ATOMIC_RELAXED);
    if (bytes > 0)
        __c11_atomic_fetch_add(&monitored_allocation_requests, 1,
                               __ATOMIC_RELAXED);
    return 1;
}
#endif

int guest_entry(void) {
#if defined(EMBREE_LOAD_ONLY)
    RTCDevice (*volatile factory)(const char*) = &rtcNewDevice;
    return factory ? 0 : 10;
#else
#if defined(EMBREE_SINGLE_THREAD)
    RTCDevice device = rtcNewDevice("threads=1");
#else
    RTCDevice device = rtcNewDevice(0);
#endif
    if (!device) return 10;
#if defined(EMBREE_DEVICE_ONLY)
    return 0;
#endif
#if defined(EMBREE_EXTENDED_APIS)
    rtcSetDeviceErrorFunction(device, device_error_callback,
                              &callback_context);
    rtcSetDeviceMemoryMonitorFunction(device, device_memory_callback,
                                      &callback_context);
#endif
    if (rtcGetDeviceError(device) != 0) {
        rtcReleaseDevice(device);
        return 11;
    }

    RTCScene scene = rtcNewScene(device);
    if (!scene) {
        rtcReleaseDevice(device);
        return 12;
    }
#if defined(EMBREE_BLENDER_GEOMETRY) || defined(EMBREE_DYNAMIC_UPDATE) || \
    defined(EMBREE_ARGUMENT_FILTER)
    unsigned int scene_flags = 0;
#if defined(EMBREE_DYNAMIC_UPDATE)
    scene_flags |= 1u; /* RTC_SCENE_FLAG_DYNAMIC */
#endif
#if defined(EMBREE_ARGUMENT_FILTER)
    scene_flags |= 8u; /* RTC_SCENE_FLAG_FILTER_FUNCTION_IN_ARGUMENTS */
#endif
    rtcSetSceneFlags(scene, scene_flags);
#endif
#if defined(EMBREE_BLENDER_GEOMETRY)
    rtcSetSceneBuildQuality(scene, 1);
#endif
#if defined(EMBREE_CALLBACKS)
    rtcSetSceneProgressMonitorFunction(scene, progress_callback, &progress_calls);
#endif
#if defined(EMBREE_NEW_SCENE_ONLY)
    return 0;
#endif
#if defined(EMBREE_TRIANGLE)
    RTCGeometry geometry = rtcNewGeometry(device, 0);
    if (!geometry) return 14;
#if EMBREE_GRID_SIZE > 0
    const unsigned int grid_size = EMBREE_GRID_SIZE;
    const unsigned int vertex_count = (grid_size + 1) * (grid_size + 1);
    const unsigned int triangle_count = 2 * grid_size * grid_size;
#else
    const unsigned int vertex_count = 3;
    const unsigned int triangle_count = 1;
#endif
#if EMBREE_TRIANGLE_STEP == 1
    return 0;
#endif
#if defined(EMBREE_BLENDER_GEOMETRY)
    float* vertices = 0;
    rtcSetNewGeometryBufferHostDevice(geometry, 1, 0, 0x9003,
        3 * sizeof(float), vertex_count, (void**)&vertices, 0);
#elif defined(EMBREE_EXTENDED_APIS)
    static float shared_vertices[3 * 65 * 65];
    float* vertices = shared_vertices;
    rtcSetSharedGeometryBuffer(geometry, 1, 0, 0x9003, vertices, 0,
                               3 * sizeof(float), vertex_count);
    if (rtcGetGeometryBufferData(geometry, 1, 0) != vertices) return 45;
#else
    float* vertices = (float*)rtcSetNewGeometryBuffer(
        geometry, 1, 0, 0x9003, 3 * sizeof(float), vertex_count);
#endif
#if EMBREE_TRIANGLE_STEP == 2
    return vertices ? 0 : 15;
#endif
#if defined(EMBREE_BLENDER_GEOMETRY)
    unsigned int* indices = 0;
    rtcSetNewGeometryBufferHostDevice(geometry, 0, 0, 0x5003,
        3 * sizeof(unsigned int), triangle_count, (void**)&indices, 0);
#else
    unsigned int* indices = (unsigned int*)rtcSetNewGeometryBuffer(
        geometry, 0, 0, 0x5003, 3 * sizeof(unsigned int), triangle_count);
#endif
#if EMBREE_TRIANGLE_STEP == 3
    return indices ? 0 : 15;
#endif
    if (!vertices || !indices) return 15;
#if EMBREE_GRID_SIZE > 0
    for (unsigned int y = 0; y <= grid_size; ++y) {
        for (unsigned int x = 0; x <= grid_size; ++x) {
            const unsigned int vertex = y * (grid_size + 1) + x;
            vertices[3 * vertex] = (float)x / (float)grid_size;
            vertices[3 * vertex + 1] = (float)y / (float)grid_size;
            vertices[3 * vertex + 2] = 0.0f;
        }
    }
    for (unsigned int y = 0; y < grid_size; ++y) {
        for (unsigned int x = 0; x < grid_size; ++x) {
            const unsigned int cell = y * grid_size + x;
            const unsigned int a = y * (grid_size + 1) + x;
            const unsigned int b = a + 1;
            const unsigned int c = a + grid_size + 1;
            const unsigned int d = c + 1;
            indices[6 * cell] = a;
            indices[6 * cell + 1] = b;
            indices[6 * cell + 2] = c;
            indices[6 * cell + 3] = b;
            indices[6 * cell + 4] = d;
            indices[6 * cell + 5] = c;
        }
    }
#else
    vertices[0] = 0.0f; vertices[1] = 0.0f; vertices[2] = 0.0f;
    vertices[3] = 1.0f; vertices[4] = 0.0f; vertices[5] = 0.0f;
    vertices[6] = 0.0f; vertices[7] = 1.0f; vertices[8] = 0.0f;
    indices[0] = 0; indices[1] = 1; indices[2] = 2;
#endif
#if defined(EMBREE_BLENDER_GEOMETRY) || defined(EMBREE_ARGUMENT_FILTER)
    rtcSetGeometryUserData(geometry, (void*)0x12345678);
    rtcSetGeometryEnableFilterFunctionFromArguments(geometry, 1);
#endif
#if defined(EMBREE_BLENDER_GEOMETRY)
    rtcSetGeometryBuildQuality(geometry, 1);
    rtcSetGeometryTimeStepCount(geometry, 1);
    rtcSetGeometryMask(geometry, ~0u);
    rtcUpdateGeometryBuffer(geometry, 1, 0);
#endif
    rtcCommitGeometry(geometry);
#if EMBREE_TRIANGLE_STEP == 4
    return 0;
#endif
#if defined(EMBREE_BLENDER_GEOMETRY)
    const unsigned int geometry_id = 7;
    rtcAttachGeometryByID(scene, geometry, geometry_id);
#else
    const unsigned int geometry_id = rtcAttachGeometry(scene, geometry);
#endif
    if (geometry_id == ~0u) return 16;
#if EMBREE_TRIANGLE_STEP == 5
    return 0;
#endif
    rtcReleaseGeometry(geometry);
#endif
#if defined(EMBREE_EXTENDED_APIS)
    /* Tessellation rate is valid for flat curves, not triangle geometry. */
    RTCGeometry flat_curve = rtcNewGeometry(device, 25);
    if (!flat_curve) return 46;
    rtcSetGeometryTessellationRate(flat_curve, 4.0f);
    float* curve_vertices = (float*)rtcSetNewGeometryBuffer(flat_curve,
        1, 0, 0x9004, 4 * sizeof(float), 4);
    unsigned int* curve_indices = (unsigned int*)rtcSetNewGeometryBuffer(
        flat_curve, 0, 0, 0x5001, sizeof(unsigned int), 1);
    if (!curve_vertices || !curve_indices) return 47;
    const float control_points[16] = {
        3.0f, 0.0f, 0.0f, 0.05f,
        3.0f, 0.25f, 0.0f, 0.05f,
        3.0f, 0.75f, 0.0f, 0.05f,
        3.0f, 1.0f, 0.0f, 0.05f
    };
    for (unsigned int i = 0; i < 16; ++i)
        curve_vertices[i] = control_points[i];
    curve_indices[0] = 0;
    rtcCommitGeometry(flat_curve);
    RTCScene curve_scene = rtcNewScene(device);
    if (!curve_scene) return 48;
    if (rtcAttachGeometry(curve_scene, flat_curve) == ~0u) return 49;
    rtcReleaseGeometry(flat_curve);
    rtcCommitScene(curve_scene);
    ProbeBounds curve_bounds = {0};
    rtcGetSceneBounds(curve_scene, &curve_bounds);
    if (curve_bounds.lower_x < 2.8f || curve_bounds.upper_x > 3.2f ||
        curve_bounds.upper_y < 0.9f) return 50;
    rtcReleaseScene(curve_scene);
#endif
    rtcCommitScene(scene);
#if defined(EMBREE_CALLBACKS)
    if (__c11_atomic_load(&progress_calls, __ATOMIC_RELAXED) == 0) return 44;
#endif
#if defined(EMBREE_BLENDER_GEOMETRY)
    ProbeBounds bounds = {0};
    rtcGetSceneBounds(scene, &bounds);
    if (bounds.lower_x < -0.001f || bounds.lower_x > 0.001f ||
        bounds.lower_y < -0.001f || bounds.lower_y > 0.001f ||
        bounds.upper_x < 0.999f || bounds.upper_x > 1.001f ||
        bounds.upper_y < 0.999f || bounds.upper_y > 1.001f ||
        bounds.lower_z < -0.001f || bounds.lower_z > 0.001f ||
        bounds.upper_z < -0.001f || bounds.upper_z > 0.001f) return 43;
#endif
#if defined(EMBREE_RAY_QUERY)
    ProbeRayHit rayhit = {0};
    rayhit.ray.org_x = 0.25f;
    rayhit.ray.org_y = 0.25f;
    rayhit.ray.org_z = -1.0f;
    rayhit.ray.dir_z = 1.0f;
    rayhit.ray.tfar = 100.0f;
    rayhit.ray.mask = ~0u;
    rayhit.hit.geomID = ~0u;
    rtcIntersect1(scene, &rayhit, 0);
    if (rayhit.hit.geomID != geometry_id ||
        rayhit.hit.primID >= triangle_count ||
        rayhit.ray.tfar < 0.99f || rayhit.ray.tfar > 1.01f) return 17;
    rayhit.ray.org_x = 2.0f;
    rayhit.ray.org_y = 2.0f;
    rayhit.ray.tfar = 100.0f;
    rayhit.hit.geomID = ~0u;
    rtcIntersect1(scene, &rayhit, 0);
    if (rayhit.hit.geomID != ~0u || rayhit.ray.tfar != 100.0f) return 18;
    rayhit.ray.org_x = 0.25f;
    rayhit.ray.org_y = 0.25f;
    rayhit.ray.tfar = 100.0f;
    rtcOccluded1(scene, &rayhit.ray, 0);
    if (rayhit.ray.tfar >= 0.0f) return 19;
    rayhit.ray.org_x = 2.0f;
    rayhit.ray.org_y = 2.0f;
    rayhit.ray.tfar = 100.0f;
    rtcOccluded1(scene, &rayhit.ray, 0);
    if (rayhit.ray.tfar != 100.0f) return 20;
    void* traversable = rtcGetSceneTraversable(scene);
    if (!traversable) return 33;
#if defined(EMBREE_BLENDER_GEOMETRY)
    RTCGeometry attached = rtcGetGeometry(scene, geometry_id);
    if (!attached) return 36;
    if (rtcGetGeometryBufferData(attached, 1, 0) != vertices) return 37;
    if (rtcGetGeometryUserDataFromTraversable(traversable, geometry_id) !=
        (void*)0x12345678) return 38;
#endif
    rayhit.ray.org_x = 0.25f;
    rayhit.ray.org_y = 0.25f;
    rayhit.ray.tfar = 100.0f;
    rayhit.hit.geomID = ~0u;
    rtcTraversableIntersect1(traversable, &rayhit, 0);
    if (rayhit.hit.geomID != geometry_id ||
        rayhit.hit.primID >= triangle_count ||
        rayhit.ray.tfar < 0.99f || rayhit.ray.tfar > 1.01f) return 34;
    rayhit.ray.tfar = 100.0f;
    rtcTraversableOccluded1(traversable, &rayhit.ray, 0);
    if (rayhit.ray.tfar >= 0.0f) return 35;
#if EMBREE_PACKET_MAX >= 4
    CHECK_PACKET(4, 21, 22, 23, 24);
#endif
#if EMBREE_PACKET_MAX >= 8
    CHECK_PACKET(8, 25, 26, 27, 28);
#endif
#if EMBREE_PACKET_MAX >= 16
    CHECK_PACKET(16, 29, 30, 31, 32);
#endif
#if defined(EMBREE_ARGUMENT_FILTER)
    ProbeIntersectArguments filtered_intersection = {0};
    filtered_intersection.feature_mask = ~0u;
    filtered_intersection.filter = reject_filter;
    rayhit.ray.org_x = 0.25f;
    rayhit.ray.org_y = 0.25f;
    rayhit.ray.tfar = 100.0f;
    rayhit.hit.geomID = ~0u;
    rtcIntersect1(scene, &rayhit, &filtered_intersection);
    if (rayhit.hit.geomID != ~0u || rayhit.ray.tfar != 100.0f)
        return 60;
    ProbeOccludedArguments filtered_occlusion = {0};
    filtered_occlusion.feature_mask = ~0u;
    filtered_occlusion.filter = reject_filter;
    rayhit.ray.tfar = 100.0f;
    rtcOccluded1(scene, &rayhit.ray, &filtered_occlusion);
    if (rayhit.ray.tfar != 100.0f ||
        __c11_atomic_load(&filter_calls, __ATOMIC_RELAXED) < 2 ||
        __c11_atomic_load(&filter_errors, __ATOMIC_RELAXED) != 0)
        return 61;
#endif
#endif
#if defined(EMBREE_INSTANCE)
    RTCScene parent = rtcNewScene(device);
    if (!parent) return 39;
    RTCGeometry instance = rtcNewGeometry(device, 121);
    if (!instance) return 40;
    rtcSetGeometryInstancedScene(instance, scene);
    rtcSetGeometryTimeStepCount(instance, 1);
#if !defined(EMBREE_EXTENDED_APIS)
    const float translate_x_two[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        2.0f, 0.0f, 0.0f, 1.0f
    };
#endif
#if defined(EMBREE_EXTENDED_APIS)
    const ProbeQuaternionDecomposition quaternion_transform = {
        1.0f, 1.0f, 1.0f,
        0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f, 0.0f,
        2.0f, 0.0f, 0.0f
    };
    rtcSetGeometryTransformQuaternion(instance, 0, &quaternion_transform);
#else
    rtcSetGeometryTransform(instance, 0, 0x9244, translate_x_two);
#endif
    rtcCommitGeometry(instance);
    rtcAttachGeometryByID(parent, instance, 3);
    rtcReleaseGeometry(instance);
    rtcCommitScene(parent);
    ProbeRayHit instanced_ray = {0};
    instanced_ray.ray.org_x = 2.25f;
    instanced_ray.ray.org_y = 0.25f;
    instanced_ray.ray.org_z = -1.0f;
    instanced_ray.ray.dir_z = 1.0f;
    instanced_ray.ray.tfar = 100.0f;
    instanced_ray.ray.mask = ~0u;
    instanced_ray.hit.geomID = ~0u;
    instanced_ray.hit.instID[0] = ~0u;
    rtcTraversableIntersect1(rtcGetSceneTraversable(parent), &instanced_ray, 0);
    if (instanced_ray.hit.geomID != geometry_id ||
        instanced_ray.hit.instID[0] != 3 ||
        instanced_ray.ray.tfar < 0.99f ||
        instanced_ray.ray.tfar > 1.01f) return 41;
    instanced_ray.ray.tfar = 100.0f;
    rtcTraversableOccluded1(rtcGetSceneTraversable(parent),
                            &instanced_ray.ray, 0);
    if (instanced_ray.ray.tfar >= 0.0f) return 42;
    rtcReleaseScene(parent);
#endif
#if defined(EMBREE_DYNAMIC_UPDATE)
    /* Refit an attached geometry after its creator reference was released. */
    RTCGeometry updated = rtcGetGeometry(scene, geometry_id);
    if (!updated || rtcGetGeometryBufferData(updated, 1, 0) != vertices)
        return 54;
    for (unsigned int vertex = 0; vertex < vertex_count; ++vertex)
        vertices[3 * vertex] += 3.0f;
    rtcUpdateGeometryBuffer(updated, 1, 0);
    rtcCommitGeometry(updated);
    rtcCommitScene(scene);
    ProbeBounds moved_bounds = {0};
    rtcGetSceneBounds(scene, &moved_bounds);
    if (moved_bounds.lower_x < 2.999f || moved_bounds.lower_x > 3.001f ||
        moved_bounds.upper_x < 3.999f || moved_bounds.upper_x > 4.001f)
        return 55;
    ProbeRayHit moved_ray = {0};
    moved_ray.ray.org_x = 0.25f;
    moved_ray.ray.org_y = 0.25f;
    moved_ray.ray.org_z = -1.0f;
    moved_ray.ray.dir_z = 1.0f;
    moved_ray.ray.tfar = 100.0f;
    moved_ray.ray.mask = ~0u;
    moved_ray.hit.geomID = ~0u;
    rtcIntersect1(scene, &moved_ray, 0);
    if (moved_ray.hit.geomID != ~0u || moved_ray.ray.tfar != 100.0f)
        return 56;
    moved_ray.ray.org_x = 3.25f;
    moved_ray.hit.geomID = ~0u;
    rtcIntersect1(scene, &moved_ray, 0);
    if (moved_ray.hit.geomID != geometry_id ||
        moved_ray.ray.tfar < 0.99f || moved_ray.ray.tfar > 1.01f)
        return 57;
    moved_ray.ray.tfar = 100.0f;
    rtcOccluded1(scene, &moved_ray.ray, 0);
    if (moved_ray.ray.tfar >= 0.0f) return 58;
#if EMBREE_PACKET_MAX >= 4
    static ProbeRayHit4 changed_packet;
    const int active_lanes[4] = {-1, -1, 0, 0};
    for (unsigned int lane = 0; lane < 4; ++lane) {
        changed_packet.ray.org_x[lane] = lane == 0 ? 3.25f : 0.25f;
        changed_packet.ray.org_y[lane] = 0.25f;
        changed_packet.ray.org_z[lane] = -1.0f;
        changed_packet.ray.dir_z[lane] = 1.0f;
        changed_packet.ray.tfar[lane] = lane < 2 ? 100.0f : 77.0f;
        changed_packet.ray.mask[lane] = ~0u;
        changed_packet.hit.geomID[lane] = lane < 2 ? ~0u : 0xabcdef01u;
    }
    rtcIntersect4(active_lanes, scene, &changed_packet, 0);
    if (changed_packet.hit.geomID[0] != geometry_id ||
        changed_packet.hit.geomID[1] != ~0u ||
        changed_packet.hit.geomID[2] != 0xabcdef01u ||
        changed_packet.hit.geomID[3] != 0xabcdef01u ||
        changed_packet.ray.tfar[2] != 77.0f ||
        changed_packet.ray.tfar[3] != 77.0f)
        return 59;
#endif
#endif
    const unsigned int error = rtcGetDeviceError(device);
#if defined(EMBREE_EXTENDED_APIS)
    if (__c11_atomic_load(&device_error_calls, __ATOMIC_RELAXED) != 0 ||
        __c11_atomic_load(&callback_context_errors, __ATOMIC_RELAXED) != 0)
        return 51;
    if (__c11_atomic_load(&memory_monitor_calls, __ATOMIC_RELAXED) == 0)
        return 52;
    if (__c11_atomic_load(&monitored_allocation_requests,
                          __ATOMIC_RELAXED) == 0)
        return 53;
#endif
    rtcReleaseScene(scene);
    rtcReleaseDevice(device);
    return error == 0 ? 0 : 13;
#endif
}

#if defined(_M_X64)
int main(void) { return guest_entry(); }
#endif
