This repository contains the source code to reproduce a Vulkan bug that can be
observed on the Mali-G1-Ultra MC12 GPU with driver version `54.1.0` (at least
on my own device).

The bug is that, when a command buffer is used to submit `vkCmdSetViewport`,
and any index of the would-be-set viewports is greater than `0`, it goes into a state.
In this state, subsequent draws (e.g. after a reset) submitted through this command buffer will fail drawing if the
bound pipeline turned on dynamic states of `VK_DYNAMIC_STATE_VIEWPORT` or
`VK_DYNAMIC_STATE_SCISSOR`, and with `VkPipelineViewportStateCreateInfo.viewportCount`
and `.scissorCount` set to `1`. `vkCmdClearAttachments` seems not affected.
Draw calls work if either `viewportCount` or `scissorCount` is greater than `1`.

Workarounds include not reusing the same command buffer (seriously?), or use
a `viewportCount` value greater than `1` if you've previously set multiple viewports.

You can modify the `repro_mode` field in `app/src/main/cpp/Renderer.h` to see
how the normal/buggy behavior would look like.

> Disclaimer: this repository contains source code generated with LLM, which may
> include invalid code and will have licensing issue. The author (do I count as one?)
> is also very unfamiliar with Vulkan.
