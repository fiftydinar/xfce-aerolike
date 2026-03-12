if [ "$EUID" -ne 0 ]; then
    bootc() {
        # Check if the command is already running with sudo
        if [ "$EUID" -eq 0 ]; then
            /usr/bin/bootc "$@"
        else
          if [ "$1" = "update" ] || [ "$1" = "upgrade" ] || [ "$1" = "status" ]; then
            sudo /usr/bin/bootc "$@"
          else
            /usr/bin/bootc "$@"
          fi
        fi
    }
fi