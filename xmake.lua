add_rules("mode.debug", "mode.release")

add_requires("liburing")

target("async-rt")
   set_kind("static")
   set_languages("c11")
   set_warnings("all", "error", "extra", "pedantic")
   add_defines("_XOPEN_SOURCE=600")
   add_files(
      "src/async.c",
      "src/ds.c",
      "src/queues.c",
      "src/coro.c",
      "src/url.c"
   )

target("main")
   set_kind("binary")
   set_languages("gnu99")
   set_warnings("all", "error", "extra", "pedantic")
   add_cxflags("-Wno-pointer-arith")
   add_files("src/main.c") -- , "src/io_uring.c")
   add_deps("async-rt")
   add_packages("liburing")
