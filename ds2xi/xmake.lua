set_allowedplats("windows", "mingw")
set_languages("c++23")

target("hidapi", function ()
    set_kind("static")
    set_runtimes("MT")
    add_includedirs("../dualsense-demo/third_party/hidapi/hidapi", {public = true})
    add_defines("HID_API_NO_EXPORT_DEFINE", {public = true})
    add_includedirs("../dualsense-demo/third_party/hidapi/windows")
    add_defines("HIDAPI_CMAKE_V0_BUILD")
    add_files("../dualsense-demo/third_party/hidapi/windows/hid.c",
              "../dualsense-demo/third_party/hidapi/windows/hidapi_descriptor_reconstruct.c")
    if is_plat("mingw") then
        add_cflags("-fno-stack-protector", {force = true})
    else
        add_cxflags("-GS-", {force = true})
    end
end)

target("XInput1_4", function ()
    set_kind("shared")
    add_deps("hidapi")
    add_files("main.cpp", "xinput.def")
    add_includedirs("..", "../dualsense-demo/third_party/hidapi/hidapi")
    add_syslinks("shlwapi", "kernel32", "user32")
    set_runtimes("MT")
    set_exceptions("none")
    set_basename("XInput1_4")
    set_prefixname("")
    set_warnings("allextra")
    set_optimize("faster")
    if is_plat("mingw") then
        add_cxxflags("-fno-stack-protector", "-fno-rtti", {force = true})
        local entry = is_arch("i386", "x86") and "_DllMain@12" or "DllMain"
        add_shflags("-nostartfiles", "-nodefaultlibs", "-Wl,--kill-at", "-Wl,--entry=" .. entry,
                    {force = true})
    else
        add_cxflags("-GS-", {force = true})
        add_shflags("/SUBSYSTEM:WINDOWS", {force = true})
    end
    if is_mode("release") then
        set_strip("all")
    end
end)
