#!/bin/bash

# Development Helper for Rae Language Project
# This script provides help for developing Rae and manages Gemini sessions.

SESSION_FILE=".gemini_sessions"

# Colors for better UX
GREEN='\033[0m\033[32m'
BLUE='\033[0m\033[34m'
YELLOW='\033[0m\033[33m'
RED='\033[0m\033[31m'
BOLD='\033[1m'
NC='\033[0m' # No Color

function header() {
    echo -e "${BLUE}${BOLD}=== Rae Language Development Helper ===${NC}"
}

function show_help() {
    header
    echo -e "This script helps you manage the Rae language development environment."
    echo ""
    echo -e "${BOLD}USAGE:${NC}"
    echo -e "  ./how-to-dev.sh [command] [options]"
    echo ""
    echo -e "${BOLD}PROJECT COMMANDS:${NC}"
    echo -e "  ${GREEN}help${NC}               Show this help message"
    echo -e "  ${GREEN}build${NC}              Build the Rae compiler (in rae/)"
    echo -e "  ${GREEN}test${NC}               Run Rae compiler tests"
    echo -e "  ${GREEN}dashboard${NC}          Start the Rae devtools web dashboard"
    echo -e "  ${GREEN}setup${NC}              Run the initial project setup"
    echo ""
    echo -e "${BOLD}GEMINI COMMANDS:${NC}"
    echo -e "  ${BLUE}--gemini${NC}           Resume the latest session in --yolo mode"
    echo -e "  ${BLUE}--gemini list${NC}      List all available Gemini sessions"
    echo -e "  ${BLUE}--gemini <index>${NC}   Resume a specific session by index (e.g., 1, 2)"
    echo -e "  ${BLUE}--gemini <id>${NC}      Resume a specific session by ID"
    echo ""
    echo -e "${BOLD}DEVELOPMENT GUIDES:${NC}"
    echo -e "  ${YELLOW}* Compiler:${NC} The compiler source is in ${BOLD}rae/compiler/src/${NC}."
    echo -e "  ${YELLOW}* Runtime:${NC}  The C runtime is in ${BOLD}rae/compiler/runtime/${NC}."
    echo -e "  ${YELLOW}* Examples:${NC} Check ${BOLD}rae/examples/${NC} for Rae code samples."
    echo -e "  ${YELLOW}* Targets:${NC}  Rae supports ${BOLD}live${NC} (VM), ${BOLD}compiled${NC} (C), and ${BOLD}hybrid${NC} targets."
}

function build_compiler() {
    echo -e "${BLUE}Building Rae compiler...${NC}"
    (cd rae && make)
}

function run_tests() {
    echo -e "${BLUE}Running Rae compiler tests...${NC}"
    (cd rae && make test)
}

function start_dashboard() {
    echo -e "${BLUE}Starting Rae devtools dashboard...${NC}"
    if [ ! -d "rae-devtools-web/node_modules" ]; then
        echo -e "${YELLOW}node_modules not found. Running bun install...${NC}"
        (cd rae-devtools-web && bun install)
    fi
    (cd rae-devtools-web && bun run dev)
}

function run_setup() {
    echo -e "${BLUE}Running project setup...${NC}"
    ./setup.sh
}

function list_sessions() {
    echo -e "${BLUE}${BOLD}Available Gemini Sessions:${NC}"
    gemini --list-sessions
}

function start_gemini() {
    local target=$1
    if [ -z "$target" ]; then
        echo -e "${BLUE}Starting Gemini in --yolo mode (resuming latest)...${NC}"
        gemini --yolo --resume latest
    elif [ "$target" == "list" ]; then
        list_sessions
    else
        echo -e "${BLUE}Resuming Gemini session: $target${NC}"
        gemini --yolo --resume "$target"
    fi
}

# Main routing
case "$1" in
    "help"|"--help"|"-h")
        show_help
        ;;
    "build")
        build_compiler
        ;;
    "test")
        run_tests
        ;;
    "dashboard")
        start_dashboard
        ;;
    "setup")
        run_setup
        ;;
    "--gemini")
        shift
        start_gemini "$1"
        ;;
    *)
        if [ -z "$1" ]; then
            show_help
        else
            echo -e "${RED}Unknown command: $1${NC}"
            show_help
            exit 1
        fi
        ;;
esac
