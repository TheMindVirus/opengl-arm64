# opengl-arm64

I have built Mesa3D using Meson and Visual Studio 2019 for ARM64 Windows on Raspberry Pi.

Please see the output at: **./builddir/src/gallium/targets/libgl-gdi/opengl32.dll**

### Notable Fixes:

MemoryBarrier is defined as a macro in intrin0.h for ARM64 and also in Mesa3D...
```
//Alastair Cota 30/09/2020 - MemoryBarrier Fix
#ifdef MemoryBarrier
#undef MemoryBarrier
#endif
```

Visual Studio .vcxproj files when included as generated folders do not include new build targets...
```
  <ProjectConfiguration Include="debugoptimized|ARM64">
    <Configuration>debugoptimized</Configuration>
    <Platform>ARM64</Platform>
  </ProjectConfiguration>
```

### WARNING! Please do not clean the build.
Mid-build changes have been applied to ensure successful compilation.
#### This software is provided as is without warranty, with the hope that it may be useful for technological education.
