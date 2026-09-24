# Formatting syntax

Used for file names when renaming and converting, library views, and tag
scripts. It looks like foobar2000's title formatting, but isn't the same
language, so scripts from foobar2000 or Picard won't work as they are.

- `%field%` inserts a tag. A missing tag is empty.
- `$function(...)` calls a function.
- Anything else is plain text. Escape `$ % , ( ) \` with a backslash.

## Fields

| Field | |
| --- | --- |
| `%title%` `%artist%` `%album%` `%albumartist%` | |
| `%tracknumber%` `%totaltracks%` `%discnumber%` `%totaldiscs%` | |
| `%date%` `%originaldate%` `%originalyear%` | |
| `%genre%` `%composer%` `%performer%` `%conductor%` `%lyricist%` | |
| `%label%` `%catalognumber%` `%barcode%` `%isrc%` | |
| `%discsubtitle%` `%subtitle%` `%media%` `%releasetype%` `%comment%` | |
| `%anytag%` | Any other tag, by its name |

A tag with several values gives them joined with `; `.

`$info(...)` gives facts about the file: `$info(filename)`,
`$info(filename_ext)`, `$info(extension)`, `$info(directory)`,
`$info(path)`. When converting, `extension` is the new format's.

## Functions

| Function | Result |
| --- | --- |
| `$if(c,a,b)` | `a` if `c` isn't empty, else `b` |
| `$if2(a,b,…)` | the first one that isn't empty |
| `$and` `$or` `$not` | logic; empty is false |
| `$eq(a,b)` `$eqi(a,b)` `$ne(a,b)` | equal, equal ignoring case, not equal |
| `$gt` `$gte` `$lt` `$lte` | compare numbers |
| `$add` `$sub` `$mul` `$div` `$mod` `$min` `$max` | arithmetic |
| `$num(n,width)` | pad with zeros: `$num(3,2)` is `03` |
| `$left(t,n)` `$right(t,n)` | first or last `n` characters |
| `$lower` `$upper` `$trim` `$len` | |
| `$replace(t,from,to,…)` | replace text |
| `$pad(t,width,char)` | pad on the right |
| `$repeat(t,n)` `$longest(a,b,…)` | |
| `$get(name)` `$getmulti(name,i)` `$join(name,sep)` `$lenmulti(name)` | tags with several values; take the tag's name, not `%name%` |
| `$each(name)` | library views only: one branch per value |

## Examples

```text
%albumartist%/%album%/$num(%tracknumber%,2) - %title%
```
`Daikaiju/Daikaiju/03 - Escape from Nebula`

```text
%albumartist%/$left(%date%,4) %album%$if($gt(%discnumber%,1),/CD %discnumber%)/$num(%tracknumber%,2)-%title%
```
`Regina Spektor/2022 Home, before and after/01-Becoming All Alone`, with a
`CD 2` folder only from the second disc on.

```text
$if2(%albumartist%,%artist%)
```
The album artist, or the artist when there isn't one.

```text
$each(artist)
```
In a library view: a track with two artists shows under both.

The full reference, with the finer rules: [tkfmt.md](tkfmt.md).
