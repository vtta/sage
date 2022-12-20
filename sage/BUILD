package(
  default_visibility = ["//visibility:public"],
)

cc_library(
  name = "sage_io",
  hdrs = ["sage_io.h"],
  deps = [
  "//gbbs:bridge",
  "//gbbs:io",
  "//gbbs:graph",
  "//gbbs:graph_io",
  "//gbbs:macros",
  "//gbbs:vertex",
  "//pbbslib:sample_sort",
  "//pbbslib:stlalgs",
  "//pbbslib/strings:string_basics",
  ]
)

cc_library(
  name = "benchmark",
  hdrs = ["benchmark.h"],
  deps = [
  ":sage_io",
  "//gbbs:parse_command_line",
  ]
)

# main header for Sage
cc_library(
  name = "sage",
  hdrs = ["sage.h"],
  deps = [
    ":sage_io",
    ":benchmark",
    "//gbbs:gbbs",
    "//gbbs/semiasym:graph_filter",
  ]
)

load("@com_grail_bazel_compdb//:defs.bzl", "compilation_database")
load("@com_grail_bazel_output_base_util//:defs.bzl", "OUTPUT_BASE")

compilation_database(
  name = "compdb",
  targets = [
    ":sage_io",
    ":sage",
    ":benchmark",
    "//sage/benchmarks/PageRank:PageRank_main",
  ],
  output_base = OUTPUT_BASE,
)
