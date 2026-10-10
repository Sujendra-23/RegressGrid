---
name: RegressGrid
description: Compact operations interface for C++ build evidence
colors:
  primary: "#18765d"
  ink: "#253c35"
  muted: "#62746d"
  background: "#f3f5f1"
  surface: "#ffffff"
  divider: "#dce4dd"
  header: "#172b28"
  console: "#162925"
  console-text: "#d0dfd3"
  passed: "#226943"
  running: "#2b6592"
  failed: "#a4402c"
typography:
  headline:
    fontFamily: "IBM Plex Sans, sans-serif"
    fontSize: "29px"
    fontWeight: 600
    lineHeight: 1.2
    letterSpacing: "-0.03em"
  body:
    fontFamily: "IBM Plex Sans, sans-serif"
    fontSize: "14px"
  code:
    fontFamily: "IBM Plex Mono, monospace"
    fontSize: "11px"
rounded:
  control: "5px"
  panel: "8px"
spacing:
  small: "12px"
  medium: "20px"
  gap: "22px"
components:
  button-primary:
    backgroundColor: "{colors.primary}"
    textColor: "{colors.surface}"
    rounded: "{rounded.control}"
    padding: "10px 15px"
  panel:
    backgroundColor: "{colors.surface}"
    rounded: "{rounded.panel}"
---

# RegressGrid design system

## Overview

A compact operational workbench. Repository names, configuration states, and
execution evidence lead. Green identifies actions and successful outcomes;
color accompanies explicit text labels. Real measurements and honest empty
states are required by the product brief.

## Colors

Primary green belongs to actions and selected navigation. Deep green anchors
the application header and log console. White work surfaces sit on a soft
neutral ground. Status colors distinguish passed, running, queued, and failed
states with text labels and dots.

## Typography

IBM Plex Sans carries the interface; IBM Plex Mono is reserved for identifiers,
source output, checksums, and waveform values. Measurements use tabular numerals.
Primary headings are 29px desktop / 26px mobile. Operational table and console
text is deliberately dense; detailed logs scroll inside their own viewport.

## Layout

The shell is capped at 1600px with 36px desktop gutters. A persistent horizontal
navigation rail leads to Builds, Workers, and Releases. The build list is 330px
beside a flexible evidence panel; both stack below 760px. Wide tables retain
horizontal scrolling. At 1100px gutters and detail density reduce. Controls and
submission fields reflow on mobile.

## Elevation & Depth

Surfaces use a single hairline border and tonal backgrounds. There are no
decorative drop shadows. Selected build rows carry a compact inset marker.

## Shapes

Panels have modest corners; controls and status tags use tighter corners.
Tables, list rows, and the console remain rectangular to preserve scanability.

## Components

Primary actions use filled green buttons; secondary actions are quiet controls.
Status badges always include a word. Submission appears inline. The detail
panel keeps the matrix visible while tabs switch logs, comparison, and artifacts.
Waveforms use bounded SVG bus traces with downloadable full VCD files.
Focus rings, selection, hover, disabled, loading, error, and empty states are
implemented. Reduced-motion preferences disable animation.

## Do's and Don'ts

- Do display actual state and measured values.
- Do preserve configuration context while changing detail tabs.
- Do retain explicit labels alongside status color.
- Do keep long logs and wide tables in scrollable regions.
- Don't invent counts, benchmark values, or production claims.
- Don't turn the infrastructure dashboard into a marketing surface.
