# SPDX-License-Identifier: GPL-3.0-only
#
# bash completion for melody-cli (ADR-0342): its commands, their words and
# options. The speakers, lists, layouts and presets are asked of the engine
# the line names -- melody-cli __complete -- and the engines of the network.

# What melody-cli __complete says of $1, names only, to the engine the line
# names.
_melody_cli_ask() {
    command melody-cli "${_melody_cli_engine[@]}" __complete "$1" 2>/dev/null | cut -f1
}

# The lines read that start with what is typed, each one word to the shell.
_melody_cli_offer() {
    local typed="${cur//\\/}" candidate
    while IFS= read -r candidate; do
        if [[ -n "${candidate}" && "${candidate}" == "${typed}"* ]]; then
            COMPREPLY+=("$(printf '%q' "${candidate}")")
        fi
    done
}

_melody_cli_words() {
    COMPREPLY+=($(compgen -W "$*" -- "${cur}"))
}

_melody_cli_files() {
    compopt -o filenames 2>/dev/null
    COMPREPLY+=($(compgen -f -- "${cur}"))
}

_melody_cli_folders() {
    compopt -o filenames 2>/dev/null
    COMPREPLY+=($(compgen -d -- "${cur}"))
}

_melody_cli() {
    local cur prev words cword
    if declare -F _init_completion > /dev/null; then
        # host:port is one word, not three.
        _init_completion -n : || return
    else
        words=("${COMP_WORDS[@]}")
        cword="${COMP_CWORD}"
        cur="${COMP_WORDS[COMP_CWORD]}"
        prev="${COMP_WORDS[COMP_CWORD - 1]}"
    fi
    COMPREPLY=()

    # The engine options before the command go along to __complete; what
    # follows the command is its own.
    local -a _melody_cli_engine=() arguments=()
    local command="" word at=1
    while ((at < cword)); do
        word="${words[at]}"
        case "${word}" in
            --json | --keys) ;;
            --format | --fields) ((at++)) ;;
            *)
                if [[ -n "${command}" ]]; then
                    arguments+=("${word}")
                else
                    case "${word}" in
                        --server | --engine | --password | --password-file)
                            _melody_cli_engine+=("${word}" "${words[at + 1]}")
                            ((at++))
                            ;;
                        --local) _melody_cli_engine+=(--local) ;;
                        -*) ;;
                        *) command="${word}" ;;
                    esac
                fi
                ;;
        esac
        ((at++))
    done

    case "${prev}" in
        --engine)
            _melody_cli_offer < <(_melody_cli_ask engines)
            return
            ;;
        --password-file | --config)
            _melody_cli_files
            return
            ;;
        --to)
            _melody_cli_folders
            return
            ;;
        --layout)
            _melody_cli_offer < <(_melody_cli_ask layouts)
            return
            ;;
        --preset)
            _melody_cli_offer < <(_melody_cli_ask presets)
            return
            ;;
        --gain)
            _melody_cli_words none track album
            return
            ;;
        --style)
            _melody_cli_words root playlist absolute
            return
            ;;
        --server | --password | --format | --fields | --size | --cover-size | --since | --run | --key)
            return
            ;;
    esac

    if [[ -z "${command}" ]]; then
        if [[ "${cur}" == -* ]]; then
            _melody_cli_words --local --server --engine --password --password-file --json \
                --keys --format --fields --help
        else
            _melody_cli_offer < <(_melody_cli_ask commands)
        fi
        return
    fi

    local count="${#arguments[@]}" first="${arguments[0]:-}"
    case "${command}" in
        play | add | insert | upnext)
            if ((count == 0)); then
                if [[ "${command}" == play ]]; then
                    _melody_cli_words album track random list
                else
                    _melody_cli_words album track random
                fi
            elif [[ "${first}" == list && "${command}" == play ]]; then
                _melody_cli_offer < <(_melody_cli_ask lists)
            elif [[ "${first}" == random ]]; then
                ((count == 1)) && _melody_cli_words album tracks
            elif [[ "${cur}" == -* ]]; then
                _melody_cli_words --key
            fi
            ;;
        output)
            ((count == 0)) && _melody_cli_offer < <(_melody_cli_ask outputs)
            ;;
        lists)
            if ((count == 0)); then
                _melody_cli_words folder
            elif [[ "${first}" == folder ]]; then
                if ((count == 1)); then
                    _melody_cli_words default
                    _melody_cli_folders
                else
                    _melody_cli_words --style
                fi
            fi
            ;;
        lastfm)
            ((count == 0)) && _melody_cli_words love unlove
            ;;
        rating | albumrating)
            ((count == 0)) && _melody_cli_words 0 1 2 3 4 5 6 7 8 9 10
            ;;
        replaygain)
            if ((count == 0)); then
                _melody_cli_words album track
            elif [[ "${cur}" == -* ]]; then
                _melody_cli_words --key --track-gain --true-peak --sidecar
            fi
            ;;
        convert)
            if [[ "${cur}" == -* ]]; then
                _melody_cli_words --to --layout --preset --gain --since --no-replaygain \
                    --no-artwork --cover-size --dry-run --presets --layouts --help
            else
                _melody_cli_files
            fi
            ;;
        cover)
            if [[ "${cur}" == -* ]]; then
                _melody_cli_words --size
            else
                _melody_cli_files
            fi
            ;;
        together)
            if ((count == 0)); then
                _melody_cli_words host join leave status party suggest vote remove
            elif [[ "${first}" == party ]]; then
                ((count == 1)) && _melody_cli_words on off
            elif [[ "${first}" == suggest ]]; then
                _melody_cli_files
            fi
            ;;
        watch)
            if ((count == 0)); then
                _melody_cli_words --all albums
            elif [[ "${first}" == albums ]]; then
                _melody_cli_words --folders --run
            fi
            ;;
        latest)
            _melody_cli_words --since --folders
            ;;
        albums | tracks)
            _melody_cli_words --fields --keys
            ;;
        find | current)
            _melody_cli_words --format
            ;;
        update)
            _melody_cli_files
            ;;
        help)
            ((count == 0)) && _melody_cli_offer < <(_melody_cli_ask commands)
            ;;
        config)
            # The key, once: what follows it is its value.
            local named=0 index=0
            while ((index < count)); do
                case "${arguments[index]}" in
                    --engine | --config) ((index++)) ;;
                    -*) ;;
                    *) ((named++)) ;;
                esac
                ((index++))
            done
            if [[ "${cur}" == -* ]]; then
                _melody_cli_words --edit --unset --engine --config
            elif ((named == 0)); then
                _melody_cli_offer < <(_melody_cli_ask config-keys)
            fi
            ;;
    esac
}

complete -F _melody_cli melody-cli
