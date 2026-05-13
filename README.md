# lirc-multiplex

This is a server that will connect to two LIRC sockets, one for
receiving and one for transmitting, and present them as a single
socket.

The scenario is perhaps you have a USB receiver and a USB
transmitter, which requires two lircd instances, but you want the
tooling to work transparently as if you have a single device.

## Building

    $ git submodule update --init --recursive
    $ make

## Usage

Let's say you have two sockets under `/var/run/lirc`, `lircd-rx`
for your receiver and `lircd-tx` for your transmitter.  You want
to share them as `lircd`.

    $ ./lirc-multiplex -server /var/run/lirc/lircd -rx /var/run/lirc/lircd-rx -tx /var/run/lirc/lircd-tx

`-server` can also share over TCP by specifying `tcp:1234` (for port 1234).
