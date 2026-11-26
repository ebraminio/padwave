set_allowedplats("windows", "mingw")
set_languages("c++23")

target("hidapi", function ()
    set_kind("static")
    add_includedirs("hidapi/hidapi", "hidapi/windows", {public = true})
    add_defines("HID_API_NO_EXPORT_DEFINE", {public = true})
    add_defines("HIDAPI_NO_CRT", "HIDAPI_CMAKE_V0_BUILD")
    add_files("hidapi/windows/hid.c",
              "hidapi/windows/hidapi_descriptor_reconstruct.c")
    set_runtimes("none")
    set_exceptions("none")
    if is_plat("mingw") then
        add_cflags("-fno-stack-protector", {force = true})
    else
        add_cxflags("/GS-", {force = true})
    end
end)

target("XInput1_4", function ()
    set_kind("shared")
    add_deps("hidapi")
    add_files("main.cc", "xinput.def")
    add_includedirs("..", "hidapi/hidapi")
    add_syslinks("shlwapi", "kernel32", "user32")
    set_runtimes("none")
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
        add_cxflags("/GS-", {force = true})
        add_shflags("/ENTRY:DllMain", "/NODEFAULTLIB", "/SUBSYSTEM:WINDOWS", {force = true})
    end
    if is_mode("release") then
        set_strip("all")
    end
end)
