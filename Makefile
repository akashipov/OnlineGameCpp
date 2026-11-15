# Сборка и запуск разных версий игры.
#
# Примеры:
#   make help                                   — список команд
#   make build                                  — собрать все версии
#   make server-v3                              — запустить сервер v3 на порту 7777
#   make server-v3 PORT=9000                    — на другом порту
#   make client-v3                              — клиент к 127.0.0.1:7777
#   make client-v3 HOST=1.2.3.4                 — клиент к удалённому серверу
#   make client-v3 HOST=1.2.3.4 PORT=9000
#
# Готовые программы складываются в папку build/.

# ---------- Настройки (можно переопределить: make ... HOST=... PORT=...) ----------

# адрес сервера для клиента
HOST ?= 127.0.0.1
# UDP-порт
PORT ?= 7777

# Компилятор: на Mac это clang, на Linux — g++ (команда c++ есть на обоих)
CXX      := c++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2

BUILD := build

# Эти цели — команды, а не файлы
.PHONY: help build build-v2 build-v3 build-v4 build-v5 server-v2 client-v2 server-v3 client-v3 server-v4 client-v4 server-v5 client-v5 clean

# ---------- Справка (запускается по умолчанию, просто "make") ----------

help:
	@echo "Сборка:"
	@echo "  make build              собрать все версии"
	@echo "  make build-v2           собрать v2 (события + время UTC)"
	@echo "  make build-v3           собрать v3 (игра с победителем)"
	@echo "  make build-v4           собрать v4 (стрелялка с окном, нужен: brew install raylib)"
	@echo "  make build-v5           собрать v5 (v4 + prediction и интерполяция)"
	@echo ""
	@echo "Запуск (по умолчанию HOST=127.0.0.1 PORT=7777):"
	@echo "  make server-v2 [PORT=7777]"
	@echo "  make client-v2 [HOST=1.2.3.4] [PORT=7777]"
	@echo "  make server-v3 [PORT=7777]"
	@echo "  make client-v3 [HOST=1.2.3.4] [PORT=7777]"
	@echo "  make server-v4 [PORT=7777]"
	@echo "  make client-v4 [HOST=1.2.3.4] [PORT=7777]"
	@echo "  make server-v5 [PORT=7777]"
	@echo "  make client-v5 [HOST=1.2.3.4] [PORT=7777]"
	@echo ""
	@echo "  make clean              удалить папку $(BUILD)/"

build: build-v2 build-v3

# ---------- v2: событийная рассылка, время UTC, keepalive ----------

build-v2: $(BUILD)/v2_server $(BUILD)/v2_client

# Пересобирается, только если изменился main.cpp или protocol.h
$(BUILD)/v2_server: v2_events/server/main.cpp v2_events/common/protocol.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) v2_events/server/main.cpp -o $(BUILD)/v2_server

$(BUILD)/v2_client: v2_events/client/main.cpp v2_events/common/protocol.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) v2_events/client/main.cpp -o $(BUILD)/v2_client

server-v2: $(BUILD)/v2_server
	./$(BUILD)/v2_server $(PORT)

client-v2: $(BUILD)/v2_client
	./$(BUILD)/v2_client $(HOST) $(PORT)

# ---------- v3: игра "у кого число больше" с GAME_END ----------

build-v3: $(BUILD)/v3_server $(BUILD)/v3_client

$(BUILD)/v3_server: v3_with_end_game/server/main.cpp v3_with_end_game/common/protocol.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) v3_with_end_game/server/main.cpp -o $(BUILD)/v3_server

$(BUILD)/v3_client: v3_with_end_game/client/main.cpp v3_with_end_game/common/protocol.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) v3_with_end_game/client/main.cpp -o $(BUILD)/v3_client

server-v3: $(BUILD)/v3_server
	./$(BUILD)/v3_server $(PORT)

client-v3: $(BUILD)/v3_client
	./$(BUILD)/v3_client $(HOST) $(PORT)

# ---------- v4: реалтайм-стрелялка с окном (raylib) ----------
#
# Клиенту нужна библиотека raylib:  brew install raylib
# Серверу raylib НЕ нужен — его можно собирать на Linux-сервере как обычно.

ifeq ($(OS),Windows_NT)
# Windows: папка, куда распакован готовый raylib (mingw-w64). Можно переопределить:
# make build-v4 RAYLIB_PATH=D:/libs/raylib-6.2_win64_mingw-w64
RAYLIB_PATH   ?= C:/raylib/raylib-6.2_win64_mingw-w64
RAYLIB_CFLAGS = -I$(RAYLIB_PATH)/include
RAYLIB_LIBS   = -L$(RAYLIB_PATH)/lib -lraylib -lopengl32 -lgdi32 -lwinmm
# net.cpp (v5) использует Winsock на Windows
WS2_LIBS      = -lws2_32
else
# Где Homebrew хранит библиотеки (обычно /opt/homebrew).
# "=" а не ":=" — команда brew выполнится, только когда собираем клиент v4.
BREW_PREFIX   = $(shell brew --prefix 2>/dev/null)
RAYLIB_CFLAGS = -I$(BREW_PREFIX)/include
RAYLIB_LIBS   = -L$(BREW_PREFIX)/lib -lraylib
endif

build-v4: $(BUILD)/v4_server $(BUILD)/v4_client

$(BUILD)/v4_server: v4_shooter/server/main.cpp v4_shooter/common/protocol.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) v4_shooter/server/main.cpp -o $(BUILD)/v4_server

$(BUILD)/v4_client: v4_shooter/client/main.cpp v4_shooter/common/protocol.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(RAYLIB_CFLAGS) v4_shooter/client/main.cpp -o $(BUILD)/v4_client $(RAYLIB_LIBS)

server-v4: $(BUILD)/v4_server
	./$(BUILD)/v4_server $(PORT)

client-v4: $(BUILD)/v4_client
	./$(BUILD)/v4_client $(HOST) $(PORT)

# ---------- v5: v4 + client-side prediction и интерполяция ----------

build-v5: $(BUILD)/v5_server $(BUILD)/v5_client

$(BUILD)/v5_server: v5_prediction/server/main.cpp v5_prediction/common/protocol.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) v5_prediction/server/main.cpp -o $(BUILD)/v5_server

$(BUILD)/v5_client: v5_prediction/client/main.cpp v5_prediction/client/net.cpp v5_prediction/client/net.h v5_prediction/common/protocol.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(RAYLIB_CFLAGS) v5_prediction/client/main.cpp v5_prediction/client/net.cpp -o $(BUILD)/v5_client $(RAYLIB_LIBS) $(WS2_LIBS)

server-v5: $(BUILD)/v5_server
	./$(BUILD)/v5_server $(PORT)

client-v5: $(BUILD)/v5_client
	./$(BUILD)/v5_client $(HOST) $(PORT)

# ---------- Очистка ----------

clean:
	rm -rf $(BUILD)
