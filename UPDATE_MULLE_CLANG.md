# Updating to a new llvm/clang version

> This "should" work, but hasn't been used yet.
> Try it next time.

Assume this directory is `mulle-clang-4.0.0/src/mulle-clang` and it's a git
repository:

```
[ -d .git ] || echo "Use the git repository, not the archive"
```

## Remove old version remnants

```
cd ../.. # be in mulle-clang-4.0.0 now

# remove old products and temporaries
sudo rm -rf ./bin ./build ./include ./lib ./libexec ./share

# remove old source
sudo rm -rf ./src/llvm
```


## Update mulle-clang with new llvm/clang version

Assume you have 4.0.0 and want to upgrade to 4.1.0. The branch scheme is
assumed to be :

LLVM       |  Mulle
-----------+-----------
release_39 | mulle_objclang_39
release_40 | mulle_objclang_40
release_41 | mulle_objclang_41
...        | ...


So what we do is, squash the current commits into one on a new branch (keeping
the old branch and commits intact though). Then we grab the new stuff off from
llvm. Since llvm branches are not that useful, rebasing often doesn't work, so
we cherrypick. Which is pretty much the same and we even get to keep some
history in the old branch.


```
OLD_LLVM_BRANCH=release_40
OLD_MULLE_DEV_BRANCH=mulle_objclang_40
NEW_LLVM_BRANCH=release_41
NEW_MULLE_DEV_BRANCH=mulle_objclang_41

(
   #
   # be in mulle-clang-4.0.0
   # get new version from LLVM (github)
   #
   cd ./src/mulle-clang

   git remote add llvm https://github.com/llvm-mirror/clang.git
   git fetch llvm

   # find the place we forked from last time
   ancestor="`git merge-base "${OLD_LLVM_BRANCH}" "${OLD_MULLE_DEV_BRANCH}"`"
   [ -z "${anchestor}" ] && echo "No common ancestor found" && exit 1

   # create a new temporary branch to contain squashed patchset
   git checkout -b "tmp_${NEW_MULLE_DEV_BRANCH}" "${ancestor}"

   #
   # squash everything into new branch
   # this helps weed out re-edits and commits that weren't useful
   # easing the conflict resolution
   #
   git merge --squash "tmp_${OLD_MULLE_DEV_BRANCH}"

   #
   # Now get the new stuff
   #
   git checkout -b "${NEW_MULLE_DEV_BRANCH}" "${NEW_LLVM_BRANCH}"
   git cherrypick "tmp_${OLD_MULLE_DEV_BRANCH}"

   #
   # resolve conflicts manually.
   # Check with grep '@mulle-objc' ... | wc -l, that all changes are present
   #
   git branch -d "tmp_${OLD_MULLE_DEV_BRANCH}"
)


