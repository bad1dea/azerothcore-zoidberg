GetProjectNameOfModuleName(${SOURCE_MODULE} MODULE_SOURCE_PROJECT_NAME)

if(TARGET modules)
  target_link_libraries(modules PUBLIC fkYAML)
endif()

if(TARGET ${MODULE_SOURCE_PROJECT_NAME})
  target_link_libraries(${MODULE_SOURCE_PROJECT_NAME} PUBLIC fkYAML)
endif()
