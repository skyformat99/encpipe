Encpipe
=======

# Usage

Encrypt a file using a password:

```sh
encpipe -e -p password -i inputfile -o outputfile
```

Decrypt a file using a password:

```sh
encpipe -d -p password -i inputfile -o outputfile
```

`-i` and `-o` can be set to `-` or omitted to read/write from the
standard input/output.

# Dependencies

[libhydrogen](https://github.com/jedisct1/libhydrogen)

# Yes

It's using passwords. Provided on the command-line. Yes, it actually
does that. I needed that. Right now. Other options will be
implemented. Next year.