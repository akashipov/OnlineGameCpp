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
.PHONY: help build build-v2 build-v3 server-v2 client-v2 server-v3 client-v3 clean

# ---------- Справка (запускается по умолчанию, просто "make") ----------

help:
	@echo "Сборка:"
	@echo "  make build              собрать все версии"
	@echo "  make build-v2           собрать v2 (события + время UTC)"
	@echo "  make build-v3           собрать v3 (игра с победителем)"
	@echo ""
	@echo "Запуск (по умолчанию HOST=127.0.0.1 PORT=7777):"
	@echo "  make server-v2 [PORT=7777]"
	@echo "  make client-v2 [HOST=1.2.3.4] [PORT=7777]"
	@echo "  make server-v3 [PORT=7777]"
	@echo "  make client-v3 [HOST=1.2.3.4] [PORT=7777]"
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

# ---------- Очистка ----------

clean:
	rm -rf $(BUILD)
