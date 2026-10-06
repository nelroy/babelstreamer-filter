# BabelStreamer Filter plugin — submission policy compliance

This document records the way in which this plugin complies with the OBS
plugin submission policy requirements.

## 1. GPL2

The plugin is licensed under GPL2.

## 2. Stable releases

Currently releasing at version 1.0.25 — this is about the 10th stable
release. New releases have been cosmetic enhancements and feature requests
from the test team.

## 3. Download link

The source code is hosted on GitHub. The compiled download is hosted from
the BabelStreamer website. This is done because, although the plugin and
the captioning functionality it provides are free, it also works as a
front end to a streaming translation service which is subscriber-based,
and we want to keep control of the download pipeline.

## 4. Resource description

We believe this is well defined.

## 5. AI usage

Use has been made of AI in the production of this plugin, but in a
responsible and careful fashion, mainly to generate first-draft frameworks
of some modules and for automation of tedious supporting files such as test
scripts and makefiles. Where code generation has been done by AI:

- All generated code has been used as a "starter" for manually written
  modules, rather than the actual end product. Every line has been gone over
  and usually rewritten by hand.
- API calls have been written or verified manually.
- The plugin has gone through extensive alpha and beta testing with
  several live users under many different scenarios.
- Final code review is always performed manually.

In general the approach used is for an experienced software engineer to
use AI as a clever but fallible coding and research assistant, much as
you would treat a novice developer, which we believe to be a responsible
and defensible use of AI.
