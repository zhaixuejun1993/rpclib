
function(rpclib_msvc_support)
  if(MSVC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
      "WIN32_LEAN_AND_MEAN"
      "NOMINMAX"
      "VC_EXTRALEAN"
      "_CRT_SECURE_NO_WARNINGS"
      "_CRT_NONSTDC_NO_DEPRECATE"
      "_WIN32_WINNT=0x0600"
      "_GNU_SOURCE")
  endif()
endfunction()
