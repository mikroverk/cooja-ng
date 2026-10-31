# Overlay descriptors — an overlay as a file

A network overlay draws protocol state on the web UI's canvas
([ui-plugins.md](ui-plugins.md)): a glyph, fill, ring, badge and label per
node, links between nodes, a legend. The built-in `rpl` and `thread`
overlays are written in C. A **descriptor** is the same thing written as a
YAML (or JSON) file: it names a firmware variable, the fields of it to read,
and how their values map onto the canvas. No C, no rebuild.

```yaml
# config (version 2)
overlays: [overlays/thread.yaml]
```

`examples/overlays/thread.yaml` is a complete one: OpenThread's MLE state in
OTNS's colours, router links included. It draws exactly what the built-in
`thread` overlay draws, and `tools/check-plugin.sh` holds it to that.

## Editing in the web UI

**Overlays** (in the toolbar when the page may edit the run: loopback, or
`--ui-edit`) opens the overlay editor: the descriptors drawn now, their text
editable. **Apply** sends the text to the simulator, which checks it and
draws it at once (paused or not), replacing the overlay with the same
`overlay:` id; an error comes back with its line and reason. **Remove**
takes an overlay off, **New** starts from a template, **Open...** loads a
file, and **Download** saves the text to list under `overlays:` in a
config. The same works in the browser build, so the site's live demos can
be redrawn by anyone watching them.

The editor speaks three WebSocket commands, `overlay-list`,
`overlay-apply` (`yaml`) and `overlay-remove` (`id`); every page hears the
answer, `{"type":"overlays","list":[{id,title,yaml}],"applied"?,"error"?}`,
with the asking page's `token`.

## The parts

```yaml
overlay: thread-descriptor            # the panel id (required)
title: "Thread (descriptor)"          # the panel's heading
symbol: _ZN2ot12gInstanceRawE         # the variable (required; mangled for C++)
struct: Instance                      # its type (required): the layouts are for it

fields:                               # name: member path in the struct (required)
  role: mMle.mRole
  mode: mMle.mDeviceMode.mMode
  rloc16: mMle.mRloc16
  partition: mMle.mLeaderData.mPartitionId
  parent: mMle.mParent.mRloc16
  router_count: mMle.mRouterTable.mRouters.mLength

key: rloc16                           # the field links point at

lists:                                # array members, each on one line
  routers: { at: mMle.mRouterTable.mRouters.mElements, count: router_count, struct: Router, fields: { router_rloc16: mRloc16, router_state: mState } }

node:                                 # how a node is drawn (all optional)
  show:  { field: role, gt: 0 }       # a condition: drawn only when it holds
  glyph: { field: role, map: { 3: hex, 4: hex }, default: circle }
  ring:  { field: role, map: { 2: "#4caf50", 3: "#1565c0", 4: "#c62828" } }
  ring_dash: { field: mode, mask: 8, eq: 0 }        # a condition
  fill:  { field: partition, color: id }
  badge: { field: role, map: { 2: C, 3: R, 4: L } }
  label: "{rloc16:04X}"
  actions: { field: role, map: { 3: [ ["Become leader", "state leader"] ] } }

links:
  - { from: parent, style: parent, color: "#8bc34a", when: { field: role, eq: 2 } }
  - { list: routers, to: router_rloc16, once: true, style: router, color: "#1976d2",
      when: { field: router_state, eq: 7 } }

legend:                               # rows for the panel box
  - [ring, "role: red leader, blue router, green child"]
counts:                               # the panel's summary line: "1 leader, 4 routers"
  - { label: [leader, leaders], when: { field: role, eq: 4 } }   # or one word

layouts: [...]                        # generated, see below
```

Every value is read as an unsigned number of up to 4 bytes, bitfields
included.

### Values

A node attribute (`glyph`, `fill`, `ring`, `badge`, `label`) is one of:

| Form | Gives |
|---|---|
| `"text {name} {name:04X}"` | the text, each `{field}` replaced by its value (`printf` width and `d`/`u`/`x`/`X`/`o`) |
| `{ field, map: { value: result, ... }, default }` | the result for the field's value; map keys may be decimal or `0x` hex |
| `{ field, color: id }` | the value as an RGB colour (`#rrggbb` of its low 24 bits — OTNS colours partitions this way) |
| `{ field, color: hash }` | the value hashed into a ten-colour palette |
| `{ cases: [ { when, value }, ... ], ... }` | the first `value` whose `when` holds, else the rest of the rule |

Any of them may take `mask:` to apply a bit mask to the field first.

`actions` is a list of `[label, command]` pairs (shown on the node's
right-click menu and sent to its console; a command may hold several lines),
or `{ field, map, default }` / `cases` giving such lists.

### Conditions

`show`, `ring_dash`, `when` and `cases` take a condition:

```yaml
{ field: role, eq: 4 }            # also ne, gt, lt, in: [2, 3]
{ field: mode, mask: 8, eq: 0 }   # mask first
{ all: [ cond, cond ] }           # every one holds
{ any: [ cond, cond ] }           # at least one holds
```

### Links

- `{ from: field, ... }`: a link from the node to the node whose `key` field
  equals this node's `from` field (a child's parent RLOC16 is its parent
  router's RLOC16).
- `{ list: name, to: field, ... }`: a link per element of the node's list
  whose `when` holds — `when` sees the element's fields — to the node whose
  `key` equals the element's `to` field. `once: true` draws each pair once
  (a router table lists the link from both ends).
- `style` is `parent` (an arrow to the target), `router`, `dashed` or
  `plain`; `color` overrides the style's colour.

## Layouts

A field's offset depends on the CPU and the firmware's configuration, so it
is not written by hand. `tools/gen-struct-layout.py` reads the struct from
an image's DWARF and writes the `layouts:` block at the end of the
descriptor, one entry per distinct layout:

```sh
tools/gen-struct-layout.py --update examples/overlays/thread.yaml \
    --name ot-cli-ftd.cc2538dk path/to/ot-cli-ftd-with-dwarf
```

It takes the `struct:`, the member paths of `fields:` and the lists from the
descriptor itself (so write each list on one line), needs pyelftools, and
handles C++ classes, base classes and bitfields. At run time the overlay
picks the entry whose machine matches the node's image and whose `size`
matches the size the image's symbol table records for the variable; an image
built another way matches none, and the panel says "layout unknown" for it
rather than reading the wrong bytes. An image without DWARF can take its
rows from a DWARF build of the same source and configuration (`-g` changes
no code; `tools/check-same-program.py` confirms it).

## What it does not do (yet)

- Follow pointers: RPL's preferred parent is a pointer into a neighbour
  table, which the `rpl` overlay (C) resolves.
- Read text or byte arrays longer than 4 bytes as values.
- Learn from packets: descriptors read memory; the `-packets` overlays (C)
  read frames.

A descriptor is checked when the run starts: an unknown key, a missing
required one, or a rule naming a field that is not there stops the run with
the reason, as an unknown config key does.
