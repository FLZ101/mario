case "$0" in
    *oksh*)
        ENV=$HOME/.kshrc
        ;;
    *dash*)
        ENV=$HOME/.shinit
        ;;
esac

export ENV
