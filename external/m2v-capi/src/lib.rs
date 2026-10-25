// m2v-capi: a C ABI over metal2vulkan's public API.
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (C) 2026 the Transmute authors.
//
// This library is free software: you can redistribute it and/or modify it under the terms of the
// GNU Lesser General Public License as published by the Free Software Foundation, either version 3
// of the License, or (at your option) any later version. It is distributed WITHOUT ANY WARRANTY;
// see COPYING.LESSER and COPYING.
//
// The ABI (include/m2v_capi.h): opaque handles for a device, a library, a pipeline and a binding
// plan; plain data in and out as JSON (UTF-8, NUL-terminated); SPIR-V as word arrays. Every
// function that can fail returns null (or false) and leaves a JSON error for m2v_last_error on the
// calling thread. Strings returned to the caller are freed with m2v_string_free. No function
// unwinds into the caller: a panic becomes an error.

use std::cell::RefCell;
use std::collections::BTreeMap;
use std::ffi::{c_char, c_void, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;

use ash::vk::{self as avk, Handle};
use metal2vulkan::bind::{BindState, BindingPlan, Dispatch, DrawCall};
use metal2vulkan::device::{ApiVersion, Device, DeviceDescription, DevicePolicy};
use metal2vulkan::library::{FunctionConstantValue, FunctionConstantValues, Library};
use metal2vulkan::metal::{self, DataType, PixelFormat};
use metal2vulkan::pipeline::{
    ColorAttachmentDescriptor, ComputePipeline, ComputePipelineDescriptor, FunctionDescriptor,
    RenderPipeline, RenderPipelineDescriptor, Stage, StageModule, StageSource, VertexDescriptor,
};
use metal2vulkan::Budget;
use serde_json::{json, Value};

/// The ABI revision include/m2v_capi.h describes. A caller refuses a library of another one.
pub const ABI_VERSION: u32 = 1;

// ---- errors ----------------------------------------------------------------------------------

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::default());
}

/// A failure: its class (`refused`, `needs_input`, `invalid`, `bind`, `internal`) and detail.
struct Failure {
    kind: &'static str,
    detail: String,
}

impl Failure {
    fn invalid(detail: impl Into<String>) -> Failure {
        Failure { kind: "invalid", detail: detail.into() }
    }
}

impl From<metal2vulkan::error::Error> for Failure {
    fn from(e: metal2vulkan::error::Error) -> Failure {
        use metal2vulkan::error::Error as E;
        let kind = match &e {
            E::NeedsInput(_) => "needs_input",
            E::Unsupported(_) => "refused",
            _ => "invalid",
        };
        Failure { kind, detail: format!("{e} ({e:?})") }
    }
}

impl From<metal2vulkan::error::BindError> for Failure {
    fn from(e: metal2vulkan::error::BindError) -> Failure {
        Failure { kind: "bind", detail: format!("{e} ({e:?})") }
    }
}

impl From<serde_json::Error> for Failure {
    fn from(e: serde_json::Error) -> Failure {
        Failure::invalid(format!("JSON: {e}"))
    }
}

fn set_error(failure: &Failure) {
    let text = json!({ "kind": failure.kind, "detail": failure.detail }).to_string();
    let text = CString::new(text.replace('\0', " ")).unwrap_or_default();
    LAST_ERROR.with(|e| *e.borrow_mut() = text);
}

/// Runs `f`, turning an error or a panic into the thread's last error and `fallback`.
fn guarded<T>(fallback: T, f: impl FnOnce() -> Result<T, Failure>) -> T {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(value)) => value,
        Ok(Err(failure)) => {
            set_error(&failure);
            fallback
        }
        Err(panic) => {
            let detail = panic
                .downcast_ref::<String>()
                .cloned()
                .or_else(|| panic.downcast_ref::<&str>().map(|s| s.to_string()))
                .unwrap_or_else(|| "panic".into());
            set_error(&Failure { kind: "internal", detail });
            fallback
        }
    }
}

unsafe fn str_arg<'a>(p: *const c_char, what: &str) -> Result<&'a str, Failure> {
    if p.is_null() {
        return Err(Failure::invalid(format!("{what} is null")));
    }
    CStr::from_ptr(p)
        .to_str()
        .map_err(|_| Failure::invalid(format!("{what} is not UTF-8")))
}

unsafe fn json_arg(p: *const c_char, what: &str) -> Result<Value, Failure> {
    Ok(serde_json::from_str(str_arg(p, what)?)?)
}

fn out_string(value: &Value) -> *mut c_char {
    CString::new(value.to_string()).map_or(ptr::null_mut(), CString::into_raw)
}

fn handle<T>(value: T) -> *mut T {
    Box::into_raw(Box::new(value))
}

unsafe fn borrow<'a, T>(p: *const T, what: &str) -> Result<&'a T, Failure> {
    p.as_ref().ok_or_else(|| Failure::invalid(format!("{what} is null")))
}

/// The ABI revision of this library.
#[no_mangle]
pub extern "C" fn m2v_abi_version() -> u32 {
    ABI_VERSION
}

/// The calling thread's last error as JSON `{"kind", "detail"}`; valid until the thread's next
/// call. Empty when nothing failed yet.
#[no_mangle]
pub extern "C" fn m2v_last_error() -> *const c_char {
    LAST_ERROR.with(|e| e.borrow().as_ptr())
}

/// Frees a string this library returned.
#[no_mangle]
pub unsafe extern "C" fn m2v_string_free(s: *mut c_char) {
    if !s.is_null() {
        drop(CString::from_raw(s));
    }
}

// ---- device ----------------------------------------------------------------------------------

pub struct M2vDevice {
    device: Device,
    description: DeviceDescription,
}

/// Feature structures metal2vulkan's query chains, by structure type: their size in bytes. Every
/// member after `sType` and `pNext` is a `VkBool32` (a trailing padding word, where there is one,
/// is masked with them, which changes nothing).
fn feature_struct_size(s_type: i32) -> Option<usize> {
    macro_rules! sizes {
        ($($t:ty),* $(,)?) => {
            $(if s_type == <$t as avk::TaggedStructure>::STRUCTURE_TYPE.as_raw() {
                return Some(std::mem::size_of::<$t>());
            })*
        };
    }
    sizes!(
        avk::PhysicalDeviceVulkan11Features<'static>,
        avk::PhysicalDeviceVulkan12Features<'static>,
        avk::PhysicalDeviceVulkan13Features<'static>,
        avk::PhysicalDeviceImageRobustnessFeatures<'static>,
        avk::PhysicalDeviceSubgroupSizeControlFeatures<'static>,
        avk::PhysicalDeviceShaderDemoteToHelperInvocationFeatures<'static>,
        avk::PhysicalDeviceShaderTerminateInvocationFeatures<'static>,
        avk::PhysicalDeviceZeroInitializeWorkgroupMemoryFeatures<'static>,
        avk::PhysicalDeviceSynchronization2Features<'static>,
        avk::PhysicalDeviceMaintenance4Features<'static>,
        avk::PhysicalDeviceShaderSubgroupRotateFeaturesKHR<'static>,
        avk::PhysicalDeviceDynamicRenderingLocalReadFeaturesKHR<'static>,
        avk::PhysicalDeviceVertexAttributeDivisorFeaturesKHR<'static>,
        avk::PhysicalDeviceMaintenance5FeaturesKHR<'static>,
        avk::PhysicalDeviceRobustness2FeaturesEXT<'static>,
        avk::PhysicalDeviceShaderAtomicFloatFeaturesEXT<'static>,
        avk::PhysicalDeviceShaderAtomicFloat2FeaturesEXT<'static>,
        avk::PhysicalDeviceShaderImageAtomicInt64FeaturesEXT<'static>,
        avk::PhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR<'static>,
        avk::PhysicalDeviceShaderMaximalReconvergenceFeaturesKHR<'static>,
        avk::PhysicalDeviceShaderQuadControlFeaturesKHR<'static>,
        avk::PhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR<'static>,
        avk::PhysicalDeviceComputeShaderDerivativesFeaturesNV<'static>,
        avk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR<'static>,
        avk::PhysicalDeviceFragmentShaderInterlockFeaturesEXT<'static>,
        avk::PhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT<'static>,
        avk::PhysicalDeviceExtendedDynamicStateFeaturesEXT<'static>,
        avk::PhysicalDeviceExtendedDynamicState2FeaturesEXT<'static>,
        avk::PhysicalDeviceExtendedDynamicState3FeaturesEXT<'static>,
        avk::PhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT<'static>,
        avk::PhysicalDeviceBorderColorSwizzleFeaturesEXT<'static>,
        avk::PhysicalDeviceDepthBiasControlFeaturesEXT<'static>,
        avk::PhysicalDeviceDepthClipEnableFeaturesEXT<'static>,
        avk::PhysicalDevicePrimitiveTopologyListRestartFeaturesEXT<'static>,
        avk::PhysicalDeviceProvokingVertexFeaturesEXT<'static>,
        avk::PhysicalDeviceLineRasterizationFeaturesEXT<'static>,
        avk::PhysicalDeviceRayQueryFeaturesKHR<'static>,
        avk::PhysicalDeviceAccelerationStructureFeaturesKHR<'static>,
        avk::PhysicalDevicePortabilitySubsetFeaturesKHR<'static>,
        avk::PhysicalDeviceMeshShaderFeaturesEXT<'static>,
    );
    // Structures newer than ash 0.38's registry: (structure type, Bool32 members).
    const NEWER: &[(i32, usize)] = &[
        (1_000_574_000, 1), // VkPhysicalDeviceMaintenance8FeaturesKHR
        (1_000_579_000, 3), // VkPhysicalDeviceShaderFmaFeaturesKHR
        (1_000_572_000, 2), // VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT
        (1_000_582_000, 1), // VkPhysicalDeviceDepthClampControlFeaturesEXT
    ];
    NEWER
        .iter()
        .find(|(t, _)| *t == s_type)
        .map(|(_, n)| (16 + 4 * n + 7) & !7)
}

/// The words after `sType` and `pNext` of a feature structure at `p` of `size` bytes.
unsafe fn feature_words<'a>(p: *mut u8, size: usize) -> &'a mut [u32] {
    std::slice::from_raw_parts_mut(p.add(16) as *mut u32, (size - 16) / 4)
}

/// What the device was created with, which the query shim answers instead of the hardware.
struct Enabled {
    real_gipa: avk::PFN_vkGetInstanceProcAddr,
    real_features2: Option<avk::PFN_vkGetPhysicalDeviceFeatures2>,
    real_extensions: Option<avk::PFN_vkEnumerateDeviceExtensionProperties>,
    /// `VkPhysicalDeviceFeatures` as words.
    core: Vec<u32>,
    /// Each chained structure's words, by structure type.
    chained: BTreeMap<i32, Vec<u32>>,
    extensions: Vec<CString>,
    unknown: Option<i32>,
}

thread_local! {
    static ENABLED: RefCell<Option<Enabled>> = const { RefCell::new(None) };
}

unsafe extern "system" fn shim_features2(
    physical: avk::PhysicalDevice,
    features: *mut avk::PhysicalDeviceFeatures2<'_>,
) {
    ENABLED.with(|cell| {
        let mut cell = cell.borrow_mut();
        let Some(enabled) = cell.as_mut() else { return };
        if let Some(real) = enabled.real_features2 {
            real(physical, features);
        }
        let head = features as *mut u8;
        let core = feature_words(head, std::mem::size_of::<avk::PhysicalDeviceFeatures2>());
        for (word, on) in core.iter_mut().zip(enabled.core.iter().chain(std::iter::repeat(&0))) {
            *word &= *on;
        }
        let mut next = (*features).p_next as *mut u8;
        while !next.is_null() {
            let s_type = *(next as *const i32);
            match feature_struct_size(s_type) {
                Some(size) => {
                    let words = feature_words(next, size);
                    let on = enabled.chained.get(&s_type);
                    for (i, word) in words.iter_mut().enumerate() {
                        *word &= on.and_then(|w| w.get(i)).copied().unwrap_or(0);
                    }
                }
                None => enabled.unknown = Some(s_type),
            }
            next = *(next.add(8) as *const *mut u8);
        }
    });
}

unsafe extern "system" fn shim_extensions(
    physical: avk::PhysicalDevice,
    layer: *const c_char,
    count: *mut u32,
    properties: *mut avk::ExtensionProperties,
) -> avk::Result {
    ENABLED.with(|cell| {
        let cell = cell.borrow();
        let Some(enabled) = cell.as_ref() else { return avk::Result::ERROR_INITIALIZATION_FAILED };
        let Some(real) = enabled.real_extensions else { return avk::Result::ERROR_INITIALIZATION_FAILED };
        let mut n = 0u32;
        let r = real(physical, layer, &mut n, ptr::null_mut());
        if r != avk::Result::SUCCESS {
            return r;
        }
        let mut all = vec![avk::ExtensionProperties::default(); n as usize];
        let r = real(physical, layer, &mut n, all.as_mut_ptr());
        if r != avk::Result::SUCCESS && r != avk::Result::INCOMPLETE {
            return r;
        }
        all.truncate(n as usize);
        all.retain(|p| {
            let name = CStr::from_ptr(p.extension_name.as_ptr());
            enabled.extensions.iter().any(|e| e.as_c_str() == name)
        });
        if properties.is_null() {
            *count = all.len() as u32;
            return avk::Result::SUCCESS;
        }
        let fit = (*count as usize).min(all.len());
        ptr::copy_nonoverlapping(all.as_ptr(), properties, fit);
        *count = fit as u32;
        if fit < all.len() {
            avk::Result::INCOMPLETE
        } else {
            avk::Result::SUCCESS
        }
    })
}

unsafe extern "system" fn shim_gipa(
    instance: avk::Instance,
    name: *const c_char,
) -> avk::PFN_vkVoidFunction {
    let wanted = CStr::from_ptr(name).to_bytes();
    let real = ENABLED.with(|cell| cell.borrow().as_ref().map(|e| e.real_gipa));
    let Some(real) = real else { return None };
    match wanted {
        b"vkGetPhysicalDeviceFeatures2" | b"vkGetPhysicalDeviceFeatures2KHR" => {
            Some(std::mem::transmute::<
                unsafe extern "system" fn(avk::PhysicalDevice, *mut avk::PhysicalDeviceFeatures2<'_>),
                unsafe extern "system" fn(),
            >(shim_features2))
        }
        b"vkEnumerateDeviceExtensionProperties" => Some(std::mem::transmute::<
            unsafe extern "system" fn(
                avk::PhysicalDevice,
                *const c_char,
                *mut u32,
                *mut avk::ExtensionProperties,
            ) -> avk::Result,
            unsafe extern "system" fn(),
        >(shim_extensions)),
        _ => real(instance, name),
    }
}

/// Describes the device Alchemy created and derives metal2vulkan's profile from it.
///
/// `enabled_features` is the `VkPhysicalDeviceFeatures2` chain passed to `vkCreateDevice` and
/// `enabled_extensions` its extension names: the description records exactly those (features
/// and extensions the hardware has but the device did not enable are absent), as
/// `DeviceDescription` requires. `policy_json` is a serialized `DevicePolicy`, or null for the
/// default. Only physical-device queries run; nothing is created.
#[no_mangle]
pub unsafe extern "C" fn m2v_device_create_vulkan(
    get_instance_proc_addr: avk::PFN_vkGetInstanceProcAddr,
    instance: *mut c_void,
    physical_device: *mut c_void,
    instance_api_version: u32,
    enabled_features: *const c_void,
    enabled_extensions: *const *const c_char,
    enabled_extension_count: u32,
    policy_json: *const c_char,
) -> *mut M2vDevice {
    guarded(ptr::null_mut(), || {
        if enabled_features.is_null() {
            return Err(Failure::invalid("enabled_features is null"));
        }
        let instance = avk::Instance::from_raw(instance as u64);
        let physical = avk::PhysicalDevice::from_raw(physical_device as u64);
        let mut chained = BTreeMap::new();
        let head = enabled_features as *mut u8;
        let core = feature_words(head, std::mem::size_of::<avk::PhysicalDeviceFeatures2>()).to_vec();
        let mut next = *(head.add(8) as *const *mut u8);
        while !next.is_null() {
            let s_type = *(next as *const i32);
            let size = feature_struct_size(s_type).ok_or_else(|| {
                Failure::invalid(format!("enabled feature chain: unknown structure type {s_type}"))
            })?;
            chained.insert(s_type, feature_words(next, size).to_vec());
            next = *(next.add(8) as *const *mut u8);
        }
        let mut extensions = Vec::new();
        for i in 0..enabled_extension_count as usize {
            extensions.push(CString::from(CStr::from_ptr(*enabled_extensions.add(i))));
        }
        let real_features2 = get_instance_proc_addr(instance, c"vkGetPhysicalDeviceFeatures2".as_ptr())
            .map(|f| std::mem::transmute::<unsafe extern "system" fn(), avk::PFN_vkGetPhysicalDeviceFeatures2>(f));
        let real_extensions =
            get_instance_proc_addr(instance, c"vkEnumerateDeviceExtensionProperties".as_ptr()).map(|f| {
                std::mem::transmute::<unsafe extern "system" fn(), avk::PFN_vkEnumerateDeviceExtensionProperties>(f)
            });
        ENABLED.with(|cell| {
            *cell.borrow_mut() = Some(Enabled {
                real_gipa: get_instance_proc_addr,
                real_features2,
                real_extensions,
                core,
                chained,
                extensions,
                unknown: None,
            })
        });
        let statics = ash::StaticFn { get_instance_proc_addr: shim_gipa };
        let ash_instance = ash::Instance::load(&statics, instance);
        let described = DeviceDescription::query(
            &ash_instance,
            physical,
            ApiVersion::from_packed(instance_api_version),
        );
        let unknown = ENABLED.with(|cell| cell.borrow_mut().take().and_then(|e| e.unknown));
        if let Some(s_type) = unknown {
            return Err(Failure::invalid(format!(
                "the description's query chained feature structure type {s_type}, which the shim cannot mask"
            )));
        }
        let description = described?;
        let policy = if policy_json.is_null() {
            DevicePolicy::default()
        } else {
            serde_json::from_str(str_arg(policy_json, "policy_json")?)?
        };
        let device = Device::new(description.clone(), policy)?;
        Ok(handle(M2vDevice { device, description }))
    })
}

/// A device from a Khronos profile JSON (tools and tests without a GPU).
#[no_mangle]
pub unsafe extern "C" fn m2v_device_create_profile(
    profile_json: *const c_char,
    profile_name: *const c_char,
) -> *mut M2vDevice {
    guarded(ptr::null_mut(), || {
        let json = str_arg(profile_json, "profile_json")?;
        let name = if profile_name.is_null() { None } else { Some(str_arg(profile_name, "profile_name")?) };
        let description = DeviceDescription::from_khronos_json(json, name)?;
        let device = Device::new(description.clone(), DevicePolicy::default())?;
        Ok(handle(M2vDevice { device, description }))
    })
}

/// The device as JSON: its description, the features and extensions its compiles need enabled,
/// the neutral resources a consumer creates once, and its Metal properties.
#[no_mangle]
pub unsafe extern "C" fn m2v_device_json(device: *const M2vDevice) -> *mut c_char {
    guarded(ptr::null_mut(), || {
        let d = borrow(device, "device")?;
        Ok(out_string(&json!({
            "description": serde_json::to_value(&d.description)?,
            "required_enables": serde_json::to_value(d.device.required_enables())?,
            "neutral_kinds": serde_json::to_value(d.device.neutral_kinds())?,
            "metal_properties": format!("{:?}", d.device.metal_properties()),
        })))
    })
}

#[no_mangle]
pub unsafe extern "C" fn m2v_device_destroy(device: *mut M2vDevice) {
    if !device.is_null() {
        drop(Box::from_raw(device));
    }
}

// ---- library ---------------------------------------------------------------------------------

pub struct M2vLibrary(Library);

/// Parses a metallib (or AIR bitcode) of `size` bytes; the bytes are copied.
#[no_mangle]
pub unsafe extern "C" fn m2v_library_parse(bytes: *const u8, size: usize) -> *mut M2vLibrary {
    guarded(ptr::null_mut(), || {
        if bytes.is_null() {
            return Err(Failure::invalid("bytes is null"));
        }
        let library = Library::parse(std::slice::from_raw_parts(bytes, size))?;
        Ok(handle(M2vLibrary(library)))
    })
}

/// The library's functions: `[{"name", "stage", "constants": [{"name", "index", "type",
/// "required"}]}]`, `type` the raw `MTLDataType` (null when unknown).
#[no_mangle]
pub unsafe extern "C" fn m2v_library_json(library: *const M2vLibrary) -> *mut c_char {
    guarded(ptr::null_mut(), || {
        let l = borrow(library, "library")?;
        let functions: Vec<Value> = l
            .0
            .functions()
            .map(|f| {
                let constants: Vec<Value> = f
                    .constants
                    .iter()
                    .map(|c| {
                        json!({
                            "name": c.name,
                            "index": c.index,
                            "type": c.ty.map(DataType::as_raw),
                            "required": c.raw_flag.map(|flag| flag & 1 != 0),
                        })
                    })
                    .collect();
                json!({ "name": f.name, "stage": format!("{:?}", f.stage), "constants": constants })
            })
            .collect();
        Ok(out_string(&Value::Array(functions)))
    })
}

#[no_mangle]
pub unsafe extern "C" fn m2v_library_destroy(library: *mut M2vLibrary) {
    if !library.is_null() {
        drop(Box::from_raw(library));
    }
}

// ---- pipelines -------------------------------------------------------------------------------

pub enum M2vPipeline {
    Compute(Box<ComputePipeline>),
    Render(Box<RenderPipeline>),
}

fn u32_field(v: &Value, key: &str) -> Result<Option<u32>, Failure> {
    match v.get(key) {
        None | Some(Value::Null) => Ok(None),
        Some(n) => n
            .as_u64()
            .and_then(|n| u32::try_from(n).ok())
            .map(Some)
            .ok_or_else(|| Failure::invalid(format!("`{key}` is not a 32-bit unsigned number"))),
    }
}

fn pixel_format(raw: u32) -> Result<PixelFormat, Failure> {
    PixelFormat::try_from(raw).map_err(|e| Failure::invalid(format!("pixel format {raw}: {e}")))
}

/// `[{"index"|"name", "type": raw MTLDataType, "bytes": "<hex>"}]` as function-constant values.
fn constants(v: Option<&Value>) -> Result<FunctionConstantValues, Failure> {
    let mut out = FunctionConstantValues::defaults();
    let Some(list) = v.and_then(Value::as_array) else { return Ok(out) };
    for c in list {
        let ty = u32_field(c, "type")?.ok_or_else(|| Failure::invalid("constant without `type`"))?;
        let ty = DataType::try_from(ty).map_err(|e| Failure::invalid(format!("constant type {ty}: {e}")))?;
        let hex = c.get("bytes").and_then(Value::as_str).unwrap_or("");
        if hex.len() % 2 != 0 {
            return Err(Failure::invalid("constant `bytes` is not whole bytes of hex"));
        }
        let bytes = (0..hex.len())
            .step_by(2)
            .map(|i| u8::from_str_radix(&hex[i..i + 2], 16))
            .collect::<Result<Vec<u8>, _>>()
            .map_err(|_| Failure::invalid("constant `bytes` is not hex"))?;
        let value = FunctionConstantValue::new(ty, bytes);
        out = match (u32_field(c, "index")?, c.get("name").and_then(Value::as_str)) {
            (Some(index), _) => out.with_index(index, value),
            (None, Some(name)) => out.with_name(name, value),
            (None, None) => return Err(Failure::invalid("constant without `index` or `name`")),
        };
    }
    Ok(out)
}

fn function(
    library: &Library,
    desc: &Value,
    name_key: &str,
    constants_key: &str,
) -> Result<Option<FunctionDescriptor>, Failure> {
    let Some(name) = desc.get(name_key).and_then(Value::as_str) else { return Ok(None) };
    let mut f = FunctionDescriptor::new(library.function(name)?);
    f.constants = constants(desc.get(constants_key))?;
    Ok(Some(f))
}

fn color_attachment(v: &Value) -> Result<(usize, ColorAttachmentDescriptor), Failure> {
    let index = u32_field(v, "index")?.unwrap_or(0) as usize;
    if index >= 8 {
        return Err(Failure::invalid("color attachment index past 7"));
    }
    let format = pixel_format(u32_field(v, "format")?.ok_or_else(|| Failure::invalid("color attachment without `format`"))?)?;
    let mut c = ColorAttachmentDescriptor::new(format);
    let raw = |key: &str| u32_field(v, key);
    let factor = |r: u32| metal::BlendFactor::try_from(r).map_err(|e| Failure::invalid(format!("blend factor {r}: {e}")));
    let op = |r: u32| metal::BlendOperation::try_from(r).map_err(|e| Failure::invalid(format!("blend operation {r}: {e}")));
    if let Some(b) = v.get("blending").and_then(Value::as_bool) {
        c.blending_enabled = b;
    }
    if let Some(r) = raw("source_rgb")? { c.source_rgb_blend_factor = factor(r)?; }
    if let Some(r) = raw("destination_rgb")? { c.destination_rgb_blend_factor = factor(r)?; }
    if let Some(r) = raw("rgb_operation")? { c.rgb_blend_operation = op(r)?; }
    if let Some(r) = raw("source_alpha")? { c.source_alpha_blend_factor = factor(r)?; }
    if let Some(r) = raw("destination_alpha")? { c.destination_alpha_blend_factor = factor(r)?; }
    if let Some(r) = raw("alpha_operation")? { c.alpha_blend_operation = op(r)?; }
    if let Some(r) = raw("write_mask")? {
        c.write_mask = metal::ColorWriteMask::try_from(r).map_err(|e| Failure::invalid(format!("write mask {r}: {e}")))?;
    }
    Ok((index, c))
}

fn compile(device: &Device, library: &Library, desc: &Value) -> Result<M2vPipeline, Failure> {
    let budget = Budget::standard();
    match desc.get("kind").and_then(Value::as_str) {
        Some("compute") => {
            let kernel = function(library, desc, "function", "constants")?
                .ok_or_else(|| Failure::invalid("compute pipeline without `function`"))?;
            let source = match desc.get("variant") {
                Some(v) if !v.is_null() => {
                    let variant: metal2vulkan::cache::VariantKey = serde_json::from_value(v.clone())?;
                    let compiled = device.compile_function(&kernel, &variant.lowering_inputs(Stage::Kernel), &budget)?;
                    StageSource::Compiled(compiled)
                }
                _ => StageSource::Function(kernel),
            };
            let mut d = ComputePipelineDescriptor::new(source);
            if let Some(si) = desc.get("stage_input").filter(|v| !v.is_null()) {
                d.stage_input = Some(serde_json::from_value(si.clone())?);
            }
            d.threadgroup_size_is_multiple_of_execution_width = desc
                .get("threadgroup_size_is_multiple_of_execution_width")
                .and_then(Value::as_bool)
                .unwrap_or(false);
            d.max_total_threads_per_threadgroup = u32_field(desc, "max_total_threads_per_threadgroup")?;
            Ok(M2vPipeline::Compute(Box::new(device.compile_compute_pipeline(&d, &budget)?)))
        }
        Some("render") => {
            let vertex = function(library, desc, "vertex", "vertex_constants")?
                .ok_or_else(|| Failure::invalid("render pipeline without `vertex`"))?;
            let mut d = RenderPipelineDescriptor::new(StageSource::Function(vertex));
            d.fragment = function(library, desc, "fragment", "fragment_constants")?.map(StageSource::Function);
            if let Some(list) = desc.get("color_attachments").and_then(Value::as_array) {
                for c in list {
                    let (index, attachment) = color_attachment(c)?;
                    d.color_attachments[index] = Some(attachment);
                }
            }
            if let Some(f) = u32_field(desc, "depth_format")? { d.depth_format = Some(pixel_format(f)?); }
            if let Some(f) = u32_field(desc, "stencil_format")? { d.stencil_format = Some(pixel_format(f)?); }
            if let Some(n) = u32_field(desc, "raster_sample_count")? { d.raster_sample_count = n; }
            if let Some(b) = desc.get("rasterization_enabled").and_then(Value::as_bool) { d.rasterization_enabled = b; }
            if let Some(b) = desc.get("alpha_to_coverage").and_then(Value::as_bool) { d.alpha_to_coverage = b; }
            if let Some(b) = desc.get("alpha_to_one").and_then(Value::as_bool) { d.alpha_to_one = b; }
            if let Some(t) = u32_field(desc, "input_primitive_topology")? {
                d.input_primitive_topology = metal::PrimitiveTopologyClass::try_from(t)
                    .map_err(|e| Failure::invalid(format!("input primitive topology {t}: {e}")))?;
            }
            if let Some(vd) = desc.get("vertex_descriptor").filter(|v| !v.is_null()) {
                d.vertex_descriptor = Some(serde_json::from_value::<VertexDescriptor>(vd.clone())?);
            }
            let pipeline = match desc.get("variant") {
                Some(v) if !v.is_null() => {
                    let variant: metal2vulkan::cache::VariantKey = serde_json::from_value(v.clone())?;
                    device.compile_render_variant(&d, &variant, &budget)?
                }
                _ => device.compile_render_pipeline(&d, &budget)?,
            };
            Ok(M2vPipeline::Render(Box::new(pipeline)))
        }
        _ => Err(Failure::invalid("pipeline `kind` is neither \"compute\" nor \"render\"")),
    }
}

/// Compiles a pipeline from `descriptor_json`:
///
/// * compute: `{"kind": "compute", "function", "constants": [...], "stage_input",
///   "max_total_threads_per_threadgroup", "threadgroup_size_is_multiple_of_execution_width",
///   "variant"}`;
/// * render: `{"kind": "render", "vertex", "vertex_constants", "fragment", "fragment_constants",
///   "color_attachments": [{"index", "format", "blending", "source_rgb", "destination_rgb",
///   "rgb_operation", "source_alpha", "destination_alpha", "alpha_operation", "write_mask"}],
///   "depth_format", "stencil_format", "raster_sample_count", "rasterization_enabled",
///   "alpha_to_coverage", "alpha_to_one", "input_primitive_topology", "vertex_descriptor",
///   "variant"}`.
///
/// Metal enumerations are their raw values. A constant is `{"index" or "name", "type" (raw
/// MTLDataType), "bytes" (hex)}`. `variant` is a plan's `variant` to compile.
#[no_mangle]
pub unsafe extern "C" fn m2v_pipeline_compile(
    device: *const M2vDevice,
    library: *const M2vLibrary,
    descriptor_json: *const c_char,
) -> *mut M2vPipeline {
    guarded(ptr::null_mut(), || {
        let d = borrow(device, "device")?;
        let l = borrow(library, "library")?;
        let desc = json_arg(descriptor_json, "descriptor_json")?;
        Ok(handle(compile(&d.device, &l.0, &desc)?))
    })
}

fn stage_json(s: &StageModule) -> Result<Value, Failure> {
    Ok(json!({
        "stage": s.stage.as_raw(),
        "entry_point": s.entry_point,
        "words": s.spirv.len(),
        "specialization": serde_json::to_value(&s.specialization)?,
        "required_subgroup_size": s.required_subgroup_size,
        "stage_flags": s.stage_flags.as_raw(),
        "constant_blob": s.constant_blob.as_ref().map(|b| b.len()),
    }))
}

impl M2vPipeline {
    fn stages(&self) -> Vec<&StageModule> {
        match self {
            M2vPipeline::Compute(p) => vec![&p.stage],
            M2vPipeline::Render(p) => p.stages.iter().collect(),
        }
    }
}

/// The pipeline as JSON: `kind`, `key`, `stages` (each's stage bit, entry point, word count,
/// specialization, required subgroup size, stage flags, constant-blob size), `layout`,
/// `interface`, `dependencies`, `variant_bits`, for a compute pipeline `metal_info` (Metal's
/// per-pipeline answers), and for a render pipeline `vertex_input` and `graphics`.
#[no_mangle]
pub unsafe extern "C" fn m2v_pipeline_json(pipeline: *const M2vPipeline) -> *mut c_char {
    guarded(ptr::null_mut(), || {
        let p = borrow(pipeline, "pipeline")?;
        let stages = p.stages().into_iter().map(stage_json).collect::<Result<Vec<_>, _>>()?;
        let v = match p {
            M2vPipeline::Compute(c) => json!({
                "kind": "compute",
                "key": format!("{:?}", c.key),
                "stages": stages,
                "layout": serde_json::to_value(&c.layout)?,
                "interface": serde_json::to_value(&c.interface)?,
                "dependencies": serde_json::to_value(&c.dependencies)?,
                "variant_bits": c.variant_bits,
                "metal_info": serde_json::to_value(c.metal_info)?,
            }),
            M2vPipeline::Render(r) => json!({
                "kind": "render",
                "key": format!("{:?}", r.key),
                "stages": stages,
                "layout": serde_json::to_value(&r.layout)?,
                "interface": serde_json::to_value(&r.interface)?,
                "dependencies": serde_json::to_value(&r.dependencies)?,
                "variant_bits": r.variant_bits,
                "vertex_input": serde_json::to_value(&r.vertex_input)?,
                "graphics": serde_json::to_value(&r.graphics)?,
            }),
        };
        Ok(out_string(&v))
    })
}

/// Stage `index`'s SPIR-V words; `*words` receives the count. Owned by the pipeline.
#[no_mangle]
pub unsafe extern "C" fn m2v_pipeline_spirv(
    pipeline: *const M2vPipeline,
    index: u32,
    words: *mut usize,
) -> *const u32 {
    guarded(ptr::null(), || {
        let p = borrow(pipeline, "pipeline")?;
        let stages = p.stages();
        let s = stages.get(index as usize).ok_or_else(|| Failure::invalid("stage index out of range"))?;
        if !words.is_null() {
            *words = s.spirv.len();
        }
        Ok(s.spirv.as_ptr())
    })
}

/// Stage `index`'s constant blob (the bytes its `ConstantBlob` binding reads), or null without
/// one; `*size` receives the byte count. Owned by the pipeline.
#[no_mangle]
pub unsafe extern "C" fn m2v_pipeline_constant_blob(
    pipeline: *const M2vPipeline,
    index: u32,
    size: *mut usize,
) -> *const u8 {
    guarded(ptr::null(), || {
        let p = borrow(pipeline, "pipeline")?;
        let stages = p.stages();
        let s = stages.get(index as usize).ok_or_else(|| Failure::invalid("stage index out of range"))?;
        let blob = s.constant_blob.as_deref();
        if !size.is_null() {
            *size = blob.map_or(0, <[u8]>::len);
        }
        Ok(blob.map_or(ptr::null(), <[u8]>::as_ptr))
    })
}

#[no_mangle]
pub unsafe extern "C" fn m2v_pipeline_destroy(pipeline: *mut M2vPipeline) {
    if !pipeline.is_null() {
        drop(Box::from_raw(pipeline));
    }
}

// ---- Metal enumerations ----------------------------------------------------------------------

/// The serialized form (in BindState and the other plain data) of raw value `raw` of the Metal
/// enumeration `type_name` (`"PixelFormat"`, `"TextureType"`, ...: the type's name without its
/// `MTL` prefix), as a JSON string; null and an error for an unknown type or value.
#[no_mangle]
pub unsafe extern "C" fn m2v_metal_enum_name(type_name: *const c_char, raw: u32) -> *mut c_char {
    guarded(ptr::null_mut(), || {
        let name = str_arg(type_name, "type_name")?;
        macro_rules! lookup {
            ($($t:ident),* $(,)?) => {
                match name {
                    $(stringify!($t) => serde_json::to_value(
                        metal::$t::try_from(raw).map_err(|e| Failure::invalid(format!("{name} {raw}: {e}")))?,
                    )?,)*
                    _ => return Err(Failure::invalid(format!("no Metal enumeration {name}"))),
                }
            };
        }
        let value = lookup!(
            AttributeFormat, BlendFactor, BlendOperation, CompareFunction, CullMode, DepthClipMode,
            IndexType, PixelFormat, PrimitiveTopologyClass, PrimitiveType, SamplerAddressMode,
            SamplerBorderColor, SamplerMinMagFilter, SamplerMipFilter, SamplerReductionMode,
            StencilOperation, StepFunction, TextureSwizzle, TextureType, TriangleFillMode,
            VertexFormat, VertexStepFunction, Winding, DataType,
        );
        Ok(out_string(&value))
    })
}

/// A serialized default value of a plain-data type, to fill in rather than spell out:
/// `"BindState"`, `"StageTable"` (both empty), `"SamplerDescriptor"` (Metal's default sampler),
/// `"TextureFacts"` (a one-level 2D RGBA8Unorm texture: set the type and format), `"VariantKey"`
/// (the empty variant: a plan whose `variant` differs asks for another).
#[no_mangle]
pub unsafe extern "C" fn m2v_template(type_name: *const c_char) -> *mut c_char {
    use metal2vulkan::bind::{SamplerDescriptor, StageTable, TextureFacts};
    guarded(ptr::null_mut(), || {
        let value = match str_arg(type_name, "type_name")? {
            "BindState" => serde_json::to_value(BindState::default())?,
            "StageTable" => serde_json::to_value(StageTable::default())?,
            "SamplerDescriptor" => serde_json::to_value(SamplerDescriptor::new())?,
            "TextureFacts" => serde_json::to_value(TextureFacts::new(metal::TextureType::Type2D, PixelFormat::RGBA8Unorm))?,
            "VariantKey" => serde_json::to_value(metal2vulkan::cache::VariantKey::default())?,
            other => return Err(Failure::invalid(format!("no template {other}"))),
        };
        Ok(out_string(&value))
    })
}

// ---- binding ---------------------------------------------------------------------------------

pub struct M2vPlan(BindingPlan);

/// The shape of a serialized `BindState`: what a plan depends on, the key to memoize plans by.
#[no_mangle]
pub unsafe extern "C" fn m2v_bind_shape(state_json: *const c_char) -> *mut c_char {
    guarded(ptr::null_mut(), || {
        let state: BindState = serde_json::from_str(str_arg(state_json, "state_json")?)?;
        Ok(out_string(&serde_json::to_value(state.shape())?))
    })
}

/// Plans `pipeline` for the shape of `state_json` (a serialized `BindState`).
#[no_mangle]
pub unsafe extern "C" fn m2v_plan_create(
    pipeline: *const M2vPipeline,
    device: *const M2vDevice,
    state_json: *const c_char,
) -> *mut M2vPlan {
    guarded(ptr::null_mut(), || {
        let p = borrow(pipeline, "pipeline")?;
        let d = borrow(device, "device")?;
        let state: BindState = serde_json::from_str(str_arg(state_json, "state_json")?)?;
        let shape = state.shape();
        let plan = match p {
            M2vPipeline::Compute(c) => c.plan(&d.device, &shape)?,
            M2vPipeline::Render(r) => r.plan(&d.device, &shape)?,
        };
        Ok(handle(M2vPlan(plan)))
    })
}

/// The plan as JSON (a serialized `BindingPlan`).
#[no_mangle]
pub unsafe extern "C" fn m2v_plan_json(plan: *const M2vPlan) -> *mut c_char {
    guarded(ptr::null_mut(), || Ok(out_string(&serde_json::to_value(&borrow(plan, "plan")?.0)?)))
}

/// One dispatch: `DispatchCommands` as JSON, from a `BindState` and a `Dispatch`.
#[no_mangle]
pub unsafe extern "C" fn m2v_plan_dispatch(
    plan: *const M2vPlan,
    state_json: *const c_char,
    dispatch_json: *const c_char,
) -> *mut c_char {
    guarded(ptr::null_mut(), || {
        let p = borrow(plan, "plan")?;
        let state: BindState = serde_json::from_str(str_arg(state_json, "state_json")?)?;
        let dispatch: Dispatch = serde_json::from_str(str_arg(dispatch_json, "dispatch_json")?)?;
        Ok(out_string(&serde_json::to_value(p.0.instantiate_dispatch(&state, &dispatch)?)?))
    })
}

/// One draw: `DrawCommands` as JSON, from a `BindState` and a `DrawCall`.
#[no_mangle]
pub unsafe extern "C" fn m2v_plan_draw(
    plan: *const M2vPlan,
    state_json: *const c_char,
    draw_json: *const c_char,
) -> *mut c_char {
    guarded(ptr::null_mut(), || {
        let p = borrow(plan, "plan")?;
        let state: BindState = serde_json::from_str(str_arg(state_json, "state_json")?)?;
        let draw: DrawCall = serde_json::from_str(str_arg(draw_json, "draw_json")?)?;
        Ok(out_string(&serde_json::to_value(p.0.instantiate_draw(&state, &draw)?)?))
    })
}

/// The Vulkan sampler (a serialized `vk::SamplerDesc`) for a plan's sampler write: the bound
/// sampler `descriptor_json` (a serialized `SamplerDescriptor`), the write's use `use_json`
/// (`{"compare", "integer", "cube_gradient", "opaque_black"}`, as the write states them), and the
/// format (`VkFormat`) of the image it samples, or a negative value for none.
#[no_mangle]
pub unsafe extern "C" fn m2v_sampler_desc(
    device: *const M2vDevice,
    descriptor_json: *const c_char,
    use_json: *const c_char,
    image_format: i64,
) -> *mut c_char {
    guarded(ptr::null_mut(), || {
        let d = borrow(device, "device")?;
        let desc: metal2vulkan::bind::SamplerDescriptor =
            serde_json::from_str(str_arg(descriptor_json, "descriptor_json")?)?;
        let use_value = json_arg(use_json, "use_json")?;
        let flag = |key: &str| use_value.get(key).and_then(Value::as_bool).unwrap_or(false);
        let sampler_use = metal::sampler::SamplerUse::new(flag("compare"), flag("integer"))
            .with_cube_gradient(flag("cube_gradient"));
        let opaque_black: metal::sampler::OpaqueBlackBorder = match use_value.get("opaque_black") {
            Some(v) if !v.is_null() => serde_json::from_value(v.clone())?,
            _ => metal::sampler::OpaqueBlackBorder::Stated,
        };
        let format = u32::try_from(image_format).ok().map(|f| metal2vulkan::vk::Format::from_raw(f as i32));
        let out = metal2vulkan::bind::sampler_desc(&d.device, &desc, sampler_use, opaque_black, format)?;
        Ok(out_string(&serde_json::to_value(out)?))
    })
}

#[no_mangle]
pub unsafe extern "C" fn m2v_plan_destroy(plan: *mut M2vPlan) {
    if !plan.is_null() {
        drop(Box::from_raw(plan));
    }
}
