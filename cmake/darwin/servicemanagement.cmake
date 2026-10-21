# ServiceManagement (src/alchemy/darwin/servicemanagement/): see its README.
# See cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/servicemanagement/job_bless.cpp)
alchemy_gui_add_framework_case(servicemanagement src/alchemy/darwin/servicemanagement/tests/servicemanagement_tests.cpp)
