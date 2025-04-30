cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

# Добавление исходников Listener Head
set(LISTENER_HEAD_SOURCE
        listener-head-main.cpp
)

# Добавление заголовочных файлов
set(LISTENER_HEAD_HEADERS
        block-reception-tracker.hpp
        listener-head-options.hpp
        listener-network-config.hpp
        listener-connection-manager.hpp
        listener-head-manager.hpp
        listener-http-server.hpp
        adnl-listener-channel.hpp
        adnl-extension.hpp
        listener-head-config.hpp
)

# Добавление исполняемого файла
add_executable(ton-listener-head ${LISTENER_HEAD_SOURCE} ${LISTENER_HEAD_HEADERS})

# Установка зависимостей
target_link_libraries(ton-listener-head PUBLIC
        tdactor
        tdnet
        tdutils
        adnl
        rldp
        dht
        tl_api
        overlay
        validator
        catchain
        ton_crypto
        ton_block
)

# Настройка включаемых директорий
target_include_directories(ton-listener-head PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# Установка компилятора для C++14
set_property(TARGET ton-listener-head PROPERTY CXX_STANDARD 14)