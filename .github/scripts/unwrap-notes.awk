# Unwrap hard-wrapped Markdown: one line per paragraph or list item.
#
#   awk -f .github/scripts/unwrap-notes.awk notes.md > unwrapped.md
#
# release.yml runs the CHANGELOG.md section through this before it becomes the
# release notes.  GitHub renders a release body's line breaks as breaks, so the
# CHANGELOG's 76-column wrapping showed as a column of short lines; the
# rendered Markdown is otherwise the same (CommonMark joins a paragraph's lines
# anyway).
#
# A line continues the one before it unless it is blank, starts a list item or
# a quote line, or is a block of one line (a heading, a table row, HTML, a link
# reference definition, a rule).  A continuation loses its indentation and
# joins with one space.  Fenced code (``` or ~~~) passes through untouched,
# and a line ending in a hard break (two spaces or a backslash) is not joined
# to the next.  Indented code blocks are not recognised; CHANGELOG.md does not
# use them.
function flush() {
    if (cur != "") print cur
    cur = ""
}
/^[ \t]*(```|~~~)/ { flush(); print; fence = !fence; next }
fence { print; next }
/^[ \t]*$/ { flush(); print; next }
# One-line blocks stand alone: a heading, a table row, HTML, a link
# reference definition, a thematic break or setext underline.
/^[ \t]*#+([ \t]|$)/ || /^[ \t]*\|/ || /^[ \t]*\[[^]]+\]:/ ||
/^[ \t]*<\/?[A-Za-z][A-Za-z0-9-]*([ \t>]|\/>|$)/ ||
/^[ \t]*(-+|=+|\*+|_+)[ \t]*$/ { flush(); print; next }
# A list item or a quote line starts a new line; what follows it joins it.
# A number other than 1 cannot start a list inside a paragraph (CommonMark),
# so "3) and ..." wrapped onto a line of its own continues the line before,
# unless that is an item of a numbered list.
/^[ \t]*([-*+>])([ \t]|$)/ || /^[ \t]*0*1[.)]([ \t]|$)/ ||
(/^[ \t]*[0-9]+[.)]([ \t]|$)/ && cur ~ /^[ \t]*[0-9]+[.)]([ \t]|$)/) { flush() }
{
    if (cur == "") cur = $0
    else { sub(/^[ \t]+/, ""); cur = cur " " $0 }
    if (cur ~ /(  |\\)$/) flush()
}
END { flush() }
