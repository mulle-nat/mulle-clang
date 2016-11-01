# mulle-clang

This is an Objective-C compiler based on clang, written for the "mulle-objc"
runtime.

> See [](README.txt) for more information about clang

The compiler can be used to:

1. compile C code
2. compile Objective-C code for "mulle-objc"

It is not recommended to use it for C++, since that is not tested.
It is not recommended to use it for other Objective-C runtimes, since there
are some global changes to make things work, that could affect other runtimes.

## Operation

The compiler compiles for the "mulle-objc" runtime by default, if no flags
are given and the compiled source is Objective-C. When compiling for
"mulle-objc" the compiler will use the [meta-ABI](//www.mulle-kybernetik.com/weblog/2015/mulle_objc_meta_call_convention.html) for all method calls. The resultant `.o`
files should be linkable like any other compiled C code.

### AAM - Always Autorelease Mode


The compiler has a special mode called AAM. This changes the Objective-C
language in the following ways.

1. There is a tranformation done on selector names
   Name           | Transformed Name
   ---------------|---------------------
   alloc          | instantiate
   new            | instantiatedObject
   copy           | immutableInstance
   mutableCopy    | mutableInstance
2. You can not access instance variables directly, but just via properties (or
methods)
3. You can not do explicit memory management (like `-dealloc`, `-autorelease`,
`-release`, `-retain`, `-retainCount` etc.)

The net effect is, that you have a mode that is ARC-like, yet understandable
and much simpler.


## Additional File Types

The compiler handles `.aam` files, which enables AMM ("Always Autoreleased Mode)


## Additional Options

Name           | Description
---------------|--------------------------------------
-fobjc-aam     | Enable AAM
-fno-objc-tps  | Disable tagged pointers


## Additional compiler defined Macros

Name               | Description
-------------------|--------------------------------------
__MULLE_OBJC__     | Compiling for mulle-objc
__MULLE_OBJC_AAM__ | AAM is enabled


## Macros used in code generation


The compiler output can be tweaked with the following preprocessor macros.
All macros must be defined with a simple integer, no expressions. All of them
are optional, unless indicated otherwise. The runtime, the Objective-C Foundation
on top of the runtime and the user application, will define them.


Name                                | Description
------------------------------------|--------------------------------------
MULLE_OBJC_RUNTIME_VERSION_MAJOR    | Major version of the runtime
MULLE_OBJC_RUNTIME_VERSION_MINOR    | Minor version of the runtime
MULLE_OBJC_RUNTIME_VERSION_PATCH    | Patch version of the runtime, these values
will be stored in the emitted object file.
MULLE_OBJC_FOUNDATION_VERSION_MAJOR | Major version of the Foundation
MULLE_OBJC_FOUNDATION_VERSION_MINOR | Minor version of the Foundation
MULLE_OBJC_FOUNDATION_VERSION_PATCH | Patch version of the Foundation,
these values will be stored in the emitted object file.
MULLE_OBJC_USER_VERSION_MAJOR       | User supplied version
MULLE_OBJC_USER_VERSION_MINOR       | User supplied of the Foundation
MULLE_OBJC_USER_VERSION_PATCH       | User supplied of the Foundation,
these values will be stored in the emitted object file.
MULLE_OBJC_NO_TAGGED_POINTERS       | Disable the emission of tagged pointer code
MULLE_OBJC_FASTCLASSHASH_0          | First unique ID of a fast class
...
MULLE_OBJC_FASTCLASSHASH_63         | Last unique ID of a fast class


## Functions used in code generation

These are the runtime functions used for method calling, retain/release
management, class lookup and exception handling.

### -O2

* mulle_objc_inline_unfailing_get_or_lookup_class
* mulle_objc_class_inline_metacall_classid
* mulle_objc_object_inline_constant_methodid_call
* mulle_objc_object_retain
* mulle_objc_object_release

### -O1

* mulle_objc_inline_unfailing_get_or_lookup_class
* mulle_objc_class_inline_metacall_classid
* mulle_objc_object_constant_methodid_call

### -O0, -Os

* mulle_objc_unfailing_get_or_lookup_class
* mulle_objc_object_call
* mulle_objc_class_metacall_classid

### All

* mulle_objc_object_get_property_value
* mulle_objc_object_set_property_value
* mulle_objc_object_zone
* mulle_objc_exception_try_enter
* mulle_objc_exception_try_exit
* mulle_objc_exception_extract
* mulle_objc_exception_match

## Author

[Nat!](//www.mulle-kybernetik.com/weblog) for
[Codeon GmbH](//www.codeon.de)
