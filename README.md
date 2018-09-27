[![Codeon Gmbh](CodeonLogo.png)](//www.codeon.de)

# mulle-clang

This is an Objective-C compiler based on clang 6.0, written for the
[mulle-objc](//www.mulle-kybernetik.com/weblog/2015/mulle_objc_a_new_objective_c_.html)
runtime. It corresponds to mulle-objc-runtime v0.13 or better.

> See [README.txt](README.txt) for more information about clang

The compiler can be used to:

1. compile Objective-C code for **mulle-objc**
2. compile C code

It is not recommended to use it for C++, since that is not tested.
It is not recommended to use it for other Objective-C runtimes, since there
have been some changes, that affect other runtimes.


## Operation

The compiler compiles Objective-C source for the **mulle-objc** runtime by
default. When compiling for **mulle-objc** the compiler will use the
[meta-ABI](//www.mulle-kybernetik.com/weblog/2015/mulle_objc_meta_call_convention.html)
for all method calls. The resultant `.o` files are linkable like any
other compiled C code.


### AAM - Always Autorelease Mode

The compiler has a special mode called AAM. This changes the Objective-C
language in the following ways:

1. There is a tranformation done on selector names

    Name           | Transformed Name
    ---------------|---------------------
    alloc          | instantiate
    new            | instantiatedObject
    copy           | immutableInstance
    mutableCopy    | mutableInstance
2. You can not access instance variables directly, but must use properties (or methods)
3. You can not do explicit memory management (like `-dealloc`, `-autorelease`,
`-release`, `-retain`, `-retainCount` etc.)

The transformed methods will return objects that are autoreleased. Hence the
name of the mode. The net effect is, that you have a mode that is ARC-like, yet
understandable and much simpler.

Your ARC code may not run in AAM, but AAM code should run in ARC with no
problems. If you can't do something in AAM, put it in a category in regular
Objective-C style.


## Additional File Types

The compiler handles `.aam` files, which enables AAM ("Always Autoreleased
Mode").


## Additional Options

Name           | Description
---------------|--------------------------------------
-fobjc-aam     | Enable AAM
-fno-objc-tps  | Disable tagged pointers
-fno-objc-fmc  | Disable fast method calls


## Additional Compiler defined Macros

In the cases where there exists a `_NO_` variant, the compiler always specifies
either of both but not both together or none of the two.

Name                    | Description
------------------------|--------------------------------------
`__MULLE_OBJC__`        | Compiling for mulle-objc
`__MULLE_OBJC_AAM__`    | AAM is enabled
`__MULLE_OBJC_NO_AAM__` | AAM is not enabled
`__MULLE_OBJC_TPS__`    | TPS (tagged pointers) is enabled
`__MULLE_OBJC_NO_TPS__` | TPS is not enabled
`__MULLE_OBJC_TRT__`    | TRT (thread-local runtime) is enabled
`__MULLE_OBJC_NO_TRT__` | TRT is not enabled
`__MULLE_OBJC_FMC__`    | FMC (fast method calling) is enabled
`__MULLE_OBJC_NO_FMC__` | FMC is not enabled


## Macros used in Code Generation


The compiler output can be tweaked with the following preprocessor macros.
All macros must be defined with a simple integer, no expressions. All of them
are optional, unless indicated otherwise. The runtime, the Objective-C
Foundation on top of the runtime and the user application, will define them.


Name                                  | Description
--------------------------------------|--------------------------------------
`MULLE_OBJC_RUNTIME_VERSION_MAJOR`    | Major version of the runtime
`MULLE_OBJC_RUNTIME_VERSION_MINOR`    | Minor version of the runtime
`MULLE_OBJC_RUNTIME_VERSION_PATCH`    | Patch version of the runtime
`MULLE_OBJC_FOUNDATION_VERSION_MAJOR` | Major version of the Foundation
`MULLE_OBJC_FOUNDATION_VERSION_MINOR` | Minor version of the Foundation
`MULLE_OBJC_FOUNDATION_VERSION_PATCH` | Patch version of the Foundation
`MULLE_OBJC_USER_VERSION_MAJOR`       | User supplied version
`MULLE_OBJC_USER_VERSION_MINOR`       | User supplied of the Foundation
`MULLE_OBJC_USER_VERSION_PATCH`       | User supplied of the Foundation, all these version information values will be stored in the emitted object file.
`MULLE_OBJC_FASTCLASSHASH_0`          | First unique ID of a fast class
... | ...
`MULLE_OBJC_FASTCLASSHASH_63`         | Last unique ID of a fast class


## Functions used in Code Generation

These are the runtime functions used for method calling, retain/release management, class lookup and exception handling. They are
defined in the runtime.

### -O2

* `mulle_objc_inlinefastlookup_infraclass_nofail`
* `mulle_objc_object_inlinecall_constantmethodid`
* `_mulle_objc_object_inlinesupercall`
* `mulle_objc_object_inlineretain`
* `mulle_objc_object_inlinerelease`

### -O1

* `mulle_objc_inlinefastlookup_infraclass_nofail`
* `mulle_objc_object_call_constantmethodid`
* `_mulle_objc_object_partialinlinesupercall`

### -O0, -Os

* `mulle_objc_fastlookup_infraclass_nofail`
* `mulle_objc_object_call`
* `_mulle_objc_object_supercall`

### All

* `mulle_objc_object_get_property_value`
* `mulle_objc_object_set_property_value`
* `mulle_objc_object_zone`
* `mulle_objc_exception_tryenter`
* `mulle_objc_exception_tryexit`
* `mulle_objc_exception_extract`
* `mulle_objc_exception_match`

## Install


### OS X

You can use [homebrew](//brew.sh) to install the compiler (only):

```
brew install codeon-gmbh/software/mulle-clang
```

> If for some reason homebrew can not use the bottle, the compiler must be
> built from source. This takes a long time! On my Macbook Air the build
> took about 30 minutes, even with the prerequisite llvm downloaded as a bottle.

### Ubuntu

You can install mulle-clang with mulle-lldb via **apt-get** on Ubuntu bionic and trusty:

```
sudo apt-get update &&
sudo apt-get install curl

curl -sS https://www.codeon.de/dists/codeon-pub.asc | sudo apt-key add -
sudo echo "deb [arch=amd64] http://download.codeon.de `lsb_release -c -s` main" > /etc/apt/sources.list.d/codeon.de-main.list

sudo apt-get update &&
sudo apt-get install mulle-clang
```

#### Debian

You can install on Debian (and Ubuntu) with:

```
OS="`lsb_release -s -c`" \
curl -O -L http://download.codeon.de/dists/${OS}/main/binary-amd64/mulle-clang-6.0.0.4-${OS}-amd64.deb && \
sudo dpkg --install mulle-clang-6.0.0.4-${OS}-amd64.deb
```

Checksum         | OS     | Result
-----------------|--------|-------------
shasum -b -a 256 | trusty | 98b5364d6cb23d662813680431183b45e523a15fbca5f92909c3e79df0a3603b
shasum -b -a 256 | bionic | 373daf1e82229270b11855cf867bc21b98d129fa96cf6a03a42d7844881a9567

### FreeBSD, Windows and others

* [How to Build](BUILD_MULLE_CLANG.md)

Afterwards head on over to [mulle-objc](//github.com/mulle-objc) to get the
runtime libraries.


## Author

[Nat!](//www.mulle-kybernetik.com/weblog) for
[Codeon GmbH](//www.codeon.de)
