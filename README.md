# Comeonin
[![Hex.pm
Version](http://img.shields.io/hexpm/v/comeonin.svg)](https://hex.pm/packages/comeonin)

Password authorization (bcrypt, etc.) library for Elixir.

## Features / subjects for discussion

This library is intended to make it very straightforward for developers
to authorize users in as secure a manner as possible.

At the moment, Comeonin only supports bcrypt, and that might be the
only hashing scheme that we do support. However, we might also support
any of the following schemes if there is any demand for them.

* sha512_crypt
* pbkdf2_sha512
* scrypt

## Installation

1. Add comeonin to your `mix.exs` dependencies

  ```elixir
  defp deps do
    [ {:comeonin, "~> 0.1.0"} ]
  end
  ```

2. List `:comeonin` as an application dependency

  ```elixir
  def application do
    [applications: [:logger, :comeonin]]
  end
  ```

3. Run `mix do deps.get, compile`

## Usage

There are functions to generate a salt `Comeonin.Bcrypt.gen_salt`
and then use that salt to hash a password `Comeonin.Bcrypt.hashpw`, but there are
also the following three convenience functions (with examples):

* hashpwsalt -- generate a salt and then use that salt to hash a password

  ```elixir
  hash = Comeonin.hashpwsalt("hard2guess")
  ```

* checkpw -- check the password against the stored hash

  ```elixir
  Comeonin.checkpw("hard2guess", stored_hash)
  ```

* dummy_checkpw -- calculate a hash and return false

  ```elixir
  Comeonin.dummy_checkpw
  ```

This last function is to be used when the username cannot be found.
It is to prevent a potential attacker enumerating the users by timing
the responses.

## Documentation

http://hexdocs.pm/comeonin

## Status

The bcrypt implementation is based on the latest OpenBSD version, which
fixed a small issue that affected some passwords longer than 72 characters.
It has been thoroughly tested in a development environment, but it has
not had much testing in production. In addition, it has only been tested
on Linux.

## License

BSD. For full details, please read the LICENSE file.
