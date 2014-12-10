
Experimental branch to pull sgen out of mono and make it generally useful.

To configure, run:

```
$ ./autogen.sh --disable-nls --disable-dtrace
```

and then to start trying to build/test:

```
$ cd mono/metadata && make test-sgen
$ ./test-sgen
```
