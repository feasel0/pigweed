// Copyright 2024 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

import { css } from 'lit';

export const styles = css`
  :host {
    align-items: center;
    background-color: var(--sys-log-viewer-color-controls-bg);
    border-bottom: 1px solid var(--md-sys-color-outline-variant);
    box-sizing: border-box;
    color: var(--sys-log-viewer-color-controls-text);
    contain: style;
    display: flex;
    flex-shrink: 0;
    gap: 1rem;
    height: 3rem;
    justify-content: space-between;
    padding: 0 1rem;
    position: relative;
    --md-list-item-leading-icon-size: 1.2rem;
    --md-filled-text-field-container-shape: 32px;
    --md-filled-text-field-container-color: var(--md-sys-color-surface);
    --md-filled-text-field-hover-state-layer-color: var(
      --md-sys-color-inverse-on-surface
    );
    --md-filled-text-field-input-text-line-height: 0.75;
    --md-filled-text-field-top-space: 0.375rem;
    --md-filled-text-field-bottom-space: 0.375rem;
    --md-filled-text-field-active-indicator-color: transparent;
    --md-filled-text-field-hover-active-indicator-color: transparent;
    --md-filled-text-field-focus-active-indicator-color: transparent;
    --md-filled-text-field-input-text-placeholder-color: var(
      --md-sys-color-outline
    );
    --text-field-icon-size: 1.75rem;
  }

  :host > * {
    display: flex;
  }

  :host *[hidden] {
    display: none;
  }

  :host([searchexpanded]) .host-name {
    display: none;
  }

  :host([searchexpanded]) .toolbar {
    flex: 1 1 auto;
  }

  :host([searchexpanded]) md-filled-text-field {
    width: 100%;
  }

  .host-name {
    display: block;
    flex: 0 1 auto;
    font-size: 1.125rem;
    font-weight: 300;
    margin: 0;
    max-width: 30rem;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .toolbar {
    align-items: center;
    flex: 1 0 auto;
    gap: 0.5rem;
    justify-content: flex-end;
  }

  .actions-container {
    flex: 0 0 auto;
    position: relative;
  }

  md-filled-text-field {
    --md-icon-button-icon-size: 1.375rem;
    --md-icon-button-icon-color: var(--md-sys-color-outline);
    --md-icon-button-state-layer-height: var(--text-field-icon-size);
    --md-icon-button-state-layer-width: var(--text-field-icon-size);
    width: 25rem;
  }

  input[type='checkbox'] {
    accent-color: var(--md-sys-color-primary);
    height: 1.125rem;
    width: 1.125rem;
  }

  .col-toggle-menu {
    background-color: var(--md-sys-color-surface-container);
    border-radius: 4px;
    margin: 0;
    padding: 0.5rem 0.75rem;
    position: absolute;
    right: 0;
    z-index: 2;
  }

  md-icon-button:nth-of-type(2) {
    padding-right: calc(var(--text-field-icon-size) - 0.5rem);
  }

  md-standard-icon-button[selected] {
    background-color: var(--sys-log-viewer-color-controls-button-enabled);
    border-radius: 100%;
  }

  .col-toggle-menu-item {
    align-items: center;
    display: flex;
    height: 3rem;
    width: max-content;
  }

  .field-toggle {
    border-radius: 1.5rem;
    position: relative;
  }

  label {
    padding-left: 0.75rem;
  }
`;
