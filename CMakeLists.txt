cmake_minimum_required(VERSION 3.20)
project(armada2_research LANGUAGES CXX)

# Reserved for future non-production experiments. No injected DLL is built here.
add_custom_target(research_workspace ALL
  COMMAND ${CMAKE_COMMAND} -E echo "Static reconnaissance workspace; no production hook target"
)

