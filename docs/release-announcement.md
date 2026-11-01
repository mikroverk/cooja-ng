# After a release: announcing it

Run through this once the release workflow has published `vX.Y.Z`
(`CLAUDE.md`, "Releasing").  The posts link the browser demo first: it is
the one thing a reader can try in a few seconds.

`CITATION.cff` and the README's Citing BibTeX are not on this list: the
release PR sets them (`CLAUDE.md`, "Releasing", step 1), and the workflow
refuses a tag whose `CITATION.cff` version and date, or BibTeX version,
year and month, are not the release's.

## The release

- [ ] The release is published, marked **Latest**, and carries the three
      tarballs and `SHA256SUMS`:
      `gh release view vX.Y.Z -R mikroverk/cooja-ng` and
      `gh api repos/mikroverk/cooja-ng/releases/latest --jq .tag_name`.
- [ ] The notes read well on GitHub.  The workflow unwraps the CHANGELOG
      section to one line per paragraph and list item
      (`.github/scripts/unwrap-notes.awk`), since GitHub shows a release
      body's line breaks; fix anything left with
      `gh release edit vX.Y.Z --notes-file`.

## The site and the docs

In one PR after the release:

- [ ] `site/index.html`: the hero link (`.pill`) points at the new tag, and
      the News section gets the release at the top, with the `latest` class
      moved to it.
- [ ] `site/index.html`: the version in the `cngsim run` sample output
      (`cooja-ng X.Y.Z · RPL-UDP on two Tmote Skys`).
- [ ] `docs/contiki-ng-testing.md`: the download commands and
      `COOJA_NG_VERSION ?=` name the new tag.
- [ ] `docs/quick-start.md`: the download URL and `PATH` name the new tag,
      and the sample output its version (`cooja-ng X.Y.Z · ...`).
- [ ] After the merge, the `pages` run on main is green and
      <https://mikroverk.github.io/cooja-ng/> shows the new version; open one
      **Run it live** demo in a fresh browser window.

## The posts

- [ ] LinkedIn and X: what is new in one or two lines, the demo link, the
      release link.  Every claim from the README or the CHANGELOG section.
- [ ] The Cooja-NG thread in Contiki-NG's GitHub Discussions: a reply with
      the release, and what changed for people who run the Cooja suite with
      `SIMULATOR=cooja-ng` (the `COOJA_NG_VERSION` pin, anything under
      *Changed*).
- [ ] A day later: GitHub Insights → Traffic, to see which post brought
      visitors.
