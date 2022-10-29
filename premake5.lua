workspace "sugar"
    configurations { "Debug", "Release" }
    architecture "x86_64"
    startproject "sugar"

project "sugar"
    kind "WindowedApp"
    language "C++"

    targetdir "target/bin/"
    objdir "target/obj/"
    debugdir "data"

    warnings "Extra"
    flags { "FatalWarnings" }

    disablewarnings { "4505", "4201" }

    files {
        "src/**.h",
        "src/**.c",
        "src/**.cpp",
    }

    includedirs {
        "src",
        "extern/agility/include",
        "extern/DirectXShaderCompiler/include",
    }

    links {
        "dxgi.lib",
        "d3d12.lib",
        "extern/DirectXShaderCompiler/bin/dxcompiler.lib"
    }

    postbuildcommands {
        "{MKDIR} target/bin/d3d12",
        "{COPY} extern/agility/bin/x64/D3D12Core.dll target/bin/d3d12",
        "{COPY} extern/agility/bin/x64/d3d12SDKLayers.dll target/bin/d3d12",
        "{COPY} extern/DirectXShaderCompiler/bin/dxil.dll target/bin",
        "{COPY} extern/DirectXShaderCompiler/bin/dxcompiler.dll target/bin"
    }

    filter "configurations:Debug"
        defines { "_DEBUG" }
        symbols "On"

    filter "configurations:Release"
        defines { "NDEBUG" }
        optimize "On"