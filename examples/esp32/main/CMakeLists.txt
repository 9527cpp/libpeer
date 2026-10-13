set(srcs "app_main.c")
if(CONFIG_EXAMPLE_ENABLE_CAMERA)
  list(APPEND srcs "camera.c" "audio.c")
endif()

idf_component_register(SRCS ${srcs}
  INCLUDE_DIRS "."
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")
