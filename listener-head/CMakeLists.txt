cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

# Добавление исходников
set(LISTENER_HEAD_SOURCE
        main.cpp
        listener-head-manager-impl.cpp
        listener-connection-manager-impl.cpp
)

# Добавление заголовочных файлов
set(LISTENER_HEAD_HEADERS
        block-reception-tracker.hpp
        listener-head-manager.hpp
        listener-connection-manager.hpp
        listener-http-server.hpp
        listener-head-config.hpp
)

# Добавление исполняемого файла
add_executable(ton-listener-head ${LISTENER_HEAD_SOURCE} ${LISTENER_HEAD_HEADERS})

# Установка зависимостей
target_link_libraries(ton-listener-head PUBLIC
        adnl
        tl_api
        overlay
        dht
        tdutils
        tdactor
        tl-utils
        rldp
        keyring
        validator-disk
        http
)

# Настройка включаемых директорий
target_include_directories(ton-listener-head PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# Установка компилятора для C++20
set_property(TARGET ton-listener-head PROPERTY CXX_STANDARD 20)

# Установка исполняемого файла
install(TARGETS ton-listener-head RUNTIME DESTINATION bin)