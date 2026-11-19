add_rules("mode.debug", "mode.release")
set_languages("c11", "c++23")

target("hidapi", function ()
    set_kind("static")
    set_runtimes("none")
    add_includedirs("third_party/hidapi/hidapi", {public = true})
    add_defines("HID_API_NO_EXPORT_DEFINE", {public = true})

    if is_plat("windows") then
        add_includedirs("third_party/hidapi/windows")
        add_defines("HIDAPI_CMAKE_V0_BUILD")
        add_files("third_party/hidapi/windows/hid.c",
                  "third_party/hidapi/windows/hidapi_descriptor_reconstruct.c")
    elseif is_plat("macosx") then
        add_includedirs("third_party/hidapi/mac")
        add_files("third_party/hidapi/mac/hid.c")
        add_frameworks("IOKit", "CoreFoundation")
        add_syslinks("pthread")
    else
        add_includedirs("third_party/hidapi/linux")
        add_defines("_GNU_SOURCE")
        add_files("third_party/hidapi/linux/hid.c")
        add_syslinks("udev", "pthread")
    end
end)

target("opus", function ()
    set_kind("static")
    set_runtimes("none")
    add_includedirs("third_party/opus/include", {public = true})
    add_includedirs("third_party/opus",
                    "third_party/opus/celt",
                    "third_party/opus/silk",
                    "third_party/opus/silk/float")
    add_defines("OPUS_BUILD")
    add_defines(is_plat("windows") and "USE_ALLOCA" or "VAR_ARRAYS")
    add_files("third_party/opus/src/*.c",
              "third_party/opus/celt/*.c",
              "third_party/opus/silk/*.c",
              "third_party/opus/silk/float/*.c")
    remove_files("third_party/opus/src/*_demo.c",
                 "third_party/opus/src/*_compare.c",
                 "third_party/opus/celt/*_demo.c",
                 "third_party/opus/**/._*.c")
end)

target("dualsense-demo", function ()
    set_kind("binary")
    set_runtimes("none")
    set_exceptions("none")
    add_cxflags(is_plat("windows") and "/GR-" or "-fno-rtti", {force = true})
    add_deps("hidapi", "opus")
    add_files("dualsense-demo.cc")
    add_includedirs("..")
    add_includedirs("third_party/hidapi/hidapi", "third_party/opus/include")
    add_defines("DS_HAVE_OPUS")
    if is_plat("windows") then
        add_defines("_CRT_SECURE_NO_WARNINGS")
    elseif is_plat("macosx") then
        add_frameworks("IOKit", "CoreFoundation")
        add_syslinks("pthread")
    else
        add_syslinks("udev", "pthread")
    end
    add_tests("help", {runargs = {"--help"}})
end)
