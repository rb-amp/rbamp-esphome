# Contributing to rbAmp

Спасибо за интерес! Эти guidelines применимы ко всем public repos в [rb-amp](https://github.com/rb-amp) org.

## What kind of contributions

✅ **Welcome:**
- Bug fixes (с tests если applicable)
- Documentation improvements (typos, clarifications, examples)
- New examples / tutorials in `rbamp-examples`
- Translation improvements (FR/DE/ES/IT/PT/RU)
- ESPHome / Arduino integration enhancements
- Community projects (showcase via [forum.rbamp.com](https://forum.rbamp.com))

⚠️ **Discuss first** (open an Issue):
- Major refactors
- New API surface area
- Breaking changes
- Hardware-specific behavior changes

❌ **Out of scope:**
- Module firmware source modifications (closed-source — see [LICENSE](LICENSE))
- Hardware schematic changes (closed-source)
- Cloud backend code (private repo)

## Workflow

1. **Open an Issue** to discuss what you want to do (saves both of our time)
2. **Fork** the repo
3. **Branch** from `main` → `feature/short-description` or `fix/issue-123`
4. **Commit** with [Conventional Commits](https://www.conventionalcommits.org/) format:
   - `feat: add support for SCT-013-100A in ESPHome component`
   - `fix: handle I²C NAK during register read`
   - `docs: clarify atomic Wh latch semantics`
5. **Test** locally (if applicable)
6. **PR** with description: what / why / how to test

## Code style

- **ESPHome component:** follow [ESPHome style guide](https://esphome.io/guides/contributing.html)
- **Arduino library:** standard Arduino style, indent 2 spaces
- **Documentation (MD):** sentence case headings, code blocks with language tag, internal links relative

## Translation contributions

Documentation is auto-translated EN → FR/DE/ES/IT/PT via Claude Haiku 4.5 + glossary. RU is hand-written.

If you spot a translation issue:
1. Open an Issue с label `translation` and locale tag
2. Suggest correction in the Issue body
3. We'll update the glossary if it's a recurring term, then re-run translation

## Code of Conduct

See [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md). Tl;dr: be respectful, focus на technical merits, no harassment.

## License agreement

By submitting a PR, you agree your contribution is licensed under the same license as the repo:
- **Documentation:** CC BY-SA 4.0
- **Code (libraries):** MIT
