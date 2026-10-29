# Validating your app’s Metal API usage

Catch runtime issues in your Metal app using API Validation.

## Overview

The API Validation layer checks for code that calls the Metal API incorrectly, including errors in creating resources, encoding Metal commands, and performing other common tasks.
You can enable API Validation using the runtime diagnostics options in Xcode, or by using environment variables.

> Important: The API Validation layer has a small, but measurable, impact on CPU performance.

### Enable API Validation with environment variables

You can also enable API Validation by setting the following environment variables on your Metal app:

- MTL_DEBUG_LAYER=1:
  Enables all API Validation tests.
- MTL_DEBUG_LAYER_ERROR_MODE:
  Sets the behavior for when a debug layer error occurs.
  Possible values are `assert` (default), `ignore`, and `nslog`.
  `assert` causes the debug layer to log and then assert on error.
  `ignore` causes the debug layer to ignore errors, which may cause undefined behavior.
  `nslog` causes the debug layer to log errors using <doc://com.apple.documentation/documentation/Foundation/NSLog>, which may also cause undefined behavior.
- MTL_DEBUG_LAYER_VALIDATE_LOAD_ACTIONS=1:
  Converts any <doc://com.apple.documentation/documentation/Metal/MTLLoadAction/dontCare> to <doc://com.apple.documentation/documentation/Metal/MTLLoadAction/clear> using a fuchsia color, which you
  can use to locate and debug incorrect load action modes or assumptions on <doc://com.apple.documentation/documentation/Metal/MTLLoadAction/dontCare> behavior.
- MTL_DEBUG_LAYER_VALIDATE_STORE_ACTIONS=1:
  Writes an alternating red-and-white checkerboard into each render target with a store action of <doc://com.apple.documentation/documentation/Metal/MTLStoreAction/dontCare>,
  which you can use to debug incorrect store action modes or assumptions on <doc://com.apple.documentation/documentation/Metal/MTLStoreAction/dontCare> behavior.
- MTL_DEBUG_LAYER_VALIDATE_UNRETAINED_RESOURCES:
  This option takes a bitfield of modes to enable.
  The default is 0x1. The bitfield values are:
  - 0x1:
    Enabling this flag causes the command buffer to tag any objects bound to it, which the system doesn’t retain internally.
    This makes Metal Validation raise an error if the system deallocates a tagged object before the command buffer completes.
  - 0x2:
    Enabling this flag causes the command buffer to tag objects that it internally retains.
    This flag is generally unnecessary because the system can’t deallocate an object while the command buffer itself isn’t complete.
  - 0x4:
    Enabling this flag causes the system to treat deallocated tagged objects as errors even before committing the command buffer.
    This leads to a more immediate error (for example, in the call stack of the deallocation), which is more debuggable than at commit.
- MTL_DEBUG_LAYER_WARNING_MODE:
  Sets the behavior for when a debug layer warning occurs.
  Possible values are `assert`, `ignore` (default), `nslog`, and `oslog`.
  `assert` causes the debug layer to log and then assert on warning.
  `ignore` causes the debug layer to ignore warnings.
  `nslog` causes the debug layer to log warnings using <doc://com.apple.documentation/documentation/Foundation/NSLog>.
  `oslog` causes the debug layer to log warnings using <doc://com.apple.documentation/documentation/os/os_log>.

For a complete list of settings, run `man MetalValidation` in Terminal.
