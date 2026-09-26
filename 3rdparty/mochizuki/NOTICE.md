# The mochizuki runtime's C ABI, vendored

`LmxxfNrApi.h` and `MochizukiNrControls.h`, copied unmodified from the OptiScaler AMD NR fork
(<https://github.com/MatheusFerreiraS/neural-amd-opti>, branch `dlss-neural-rendering`, commit
`0ae1a2ec`): `OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfNrApi.h` and
`OptiScaler/dlssnr/backend/mochizuki_runtime/MochizukiNrControls.h`.

They describe `MochizukiNrRuntime.dll`, which runs mochizuki0323's Vulkan port of the network
(<https://github.com/mochizuki0323/DLSSNR-AMD>, MIT). The add-on loads that DLL at run time and
links nothing from it.
