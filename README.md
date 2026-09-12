This repository contains source code to reproduce a bug that can be observed on
the Vulkan driver versioned `54.1.0`, supplied with Android devices with the
Mali-G1-Ultra GPU.

On this driver, `vkCmdSetViewport` writes the maximum viewport index seen[^1]
into command-buffer-local internal state, which won't be cleared by `vkResetCommandBuffer`.

The bug is that, with the maximum viewport index seen greater than `0`, subsequent
draws will not succeed (but without returning error) if the bound pipeline
turned on dynamic states of `VK_DYNAMIC_STATE_VIEWPORT` or `VK_DYNAMIC_STATE_SCISSOR`,
and with `VkPipelineViewportStateCreateInfo.viewportCount` and `.scissorCount`
set to `1`. This configuration apparently selects a single viewport path that
doesn't work with `max_seen_vp_idx > 0`.

`vkCmdClearAttachments` seems not affected. Draw calls work if either `viewportCount`
or `scissorCount` is greater than `1`.

Workaround: When reusing command buffers, use a `viewportCount` value greater
than `1` if you've previously set multiple viewports.

You can modify the `repro_mode` field in `app/src/main/cpp/Renderer.h` to see
how the normal/buggy behavior would look like.

> Warning: this repository contains source code that hasn't been fully deslopped,
> which may include invalid code and will have licensing issue.

[^1]:  The stored value is actually maximum seen viewport index plus 1.
