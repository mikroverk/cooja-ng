# After a release: announcing it

Run through this once the release workflow has published `vX.Y.Z`
(`CLAUDE.md`, "Releasing").  The posts link the browser demo first: it is
the one thing a reader can try in a few seconds.

## The release

- [ ] The release is published, marked **Latest**, and carries the three
      tarballs and `SHA256SUMS`:
      `gh release view vX.Y.Z -R mikroverk/cooja-ng` and
      `gh api repos/mikroverk/cooja-ng/releases/latest --jq .tag_name`.
- [ ] The notes read well on GitHub.  GitHub keeps the CHANGELOG's line
      breaks, so a hard-wrapped section shows as short lines; unwrap the
      paragraphs and list items with `gh release edit vX.Y.Z --notes-file`
      if so.
- [ ] `CITATION.cff` has the new `version:` and `date-released:` (the release
      workflow refuses a tag otherwise), and the README's Citing BibTeX the
      new version.

## The site and the docs

In one PR after the release:

- [ ] `site/index.html`: the hero link (`.pill`) points at the new tag, and
      the News section gets the release at the top, with the `latest` class
      moved to it.
- [ ] `docs/contiki-ng-testing.md`: the download commands and
      `COOJA_NG_VERSION ?=` name the new tag.
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
