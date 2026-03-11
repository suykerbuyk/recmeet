// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

'use strict';

const API = '';

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

let currentView = 'speakers';
let speakers = [];
let meetings = [];
// Cache: meeting dir name -> speaker array
const meetingSpeakerCache = {};

// ---------------------------------------------------------------------------
// DOM helpers
// ---------------------------------------------------------------------------

const $ = (sel) => document.querySelector(sel);
const $$ = (sel) => document.querySelectorAll(sel);

function html(tag, attrs, ...children) {
  const el = document.createElement(tag);
  if (attrs) {
    for (const [k, v] of Object.entries(attrs)) {
      if (k.startsWith('on')) {
        el.addEventListener(k.slice(2), v);
      } else if (k === 'className') {
        el.className = v;
      } else {
        el.setAttribute(k, v);
      }
    }
  }
  for (const child of children) {
    if (typeof child === 'string') {
      el.appendChild(document.createTextNode(child));
    } else if (child) {
      el.appendChild(child);
    }
  }
  return el;
}

// ---------------------------------------------------------------------------
// API calls
// ---------------------------------------------------------------------------

async function api(method, path, body) {
  const opts = { method, headers: {} };
  if (body !== undefined) {
    opts.headers['Content-Type'] = 'application/json';
    opts.body = JSON.stringify(body);
  }
  const res = await fetch(API + path, opts);
  const data = await res.json();
  if (!res.ok) throw new Error(data.error || `HTTP ${res.status}`);
  return data;
}

async function fetchSpeakers() {
  speakers = await api('GET', '/api/speakers');
}

async function fetchMeetings() {
  meetings = await api('GET', '/api/meetings');
}

async function fetchMeetingSpeakers(dirName) {
  if (!meetingSpeakerCache[dirName]) {
    meetingSpeakerCache[dirName] = await api('GET', `/api/meetings/${encodeURIComponent(dirName)}/speakers`);
  }
  return meetingSpeakerCache[dirName];
}

async function deleteSpeaker(name) {
  await api('DELETE', `/api/speakers/${encodeURIComponent(name)}`);
}

async function resetAllSpeakers() {
  return await api('POST', '/api/speakers/reset');
}

async function enrollSpeaker(name, meetingDir, clusterId) {
  const result = await api('POST', '/api/speakers/enroll', {
    name, meeting_dir: meetingDir, cluster_id: clusterId
  });
  // Invalidate cache for this meeting
  delete meetingSpeakerCache[meetingDir];
  return result;
}

async function relabelSpeaker(meetingDir, clusterId, newLabel, updateProfile = true) {
  const result = await api('POST', `/api/meetings/${encodeURIComponent(meetingDir)}/speakers/relabel`, {
    cluster_id: clusterId, new_label: newLabel, update_profile: updateProfile ? 'true' : 'false'
  });
  delete meetingSpeakerCache[meetingDir];
  return result;
}

async function removeEmbedding(name, index) {
  return await api('POST', `/api/speakers/${encodeURIComponent(name)}/remove-embedding`, { index });
}

async function reprocessMeeting(meetingDir, numSpeakers) {
  return await api('POST', `/api/meetings/${encodeURIComponent(meetingDir)}/reprocess`, {
    num_speakers: numSpeakers || 0
  });
}

async function batchReidentify() {
  return await api('POST', '/api/speakers/batch-reidentify');
}

// ---------------------------------------------------------------------------
// Toast
// ---------------------------------------------------------------------------

let toastTimer = null;

function toast(msg) {
  const el = $('#toast');
  el.textContent = msg;
  el.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.remove('show'), 2500);
}

// ---------------------------------------------------------------------------
// Confirm dialog
// ---------------------------------------------------------------------------

function confirm(title, message) {
  return new Promise((resolve) => {
    const overlay = html('div', { className: 'overlay' },
      html('div', { className: 'dialog' },
        html('h3', null, title),
        html('p', null, message),
        html('div', { className: 'dialog-actions' },
          html('button', { className: 'btn', onclick: () => { overlay.remove(); resolve(false); } }, 'Cancel'),
          html('button', { className: 'btn btn-danger', onclick: () => { overlay.remove(); resolve(true); } }, 'Confirm')
        )
      )
    );
    document.body.appendChild(overlay);
  });
}

// ---------------------------------------------------------------------------
// Speakers View
// ---------------------------------------------------------------------------

function renderSpeakers() {
  const container = $('#content');
  container.innerHTML = '';

  const toolbar = html('div', { className: 'toolbar' },
    html('h2', null, 'Enrolled Speakers'),
    html('div', null,
      html('button', { className: 'btn', onclick: refreshSpeakers }, 'Refresh'),
      document.createTextNode(' '),
      html('button', { className: 'btn', onclick: handleBatchReidentify }, 'Batch Re-identify'),
      document.createTextNode(' '),
      html('button', { className: 'btn btn-danger', onclick: handleResetAll }, 'Reset All')
    )
  );
  container.appendChild(toolbar);

  if (speakers.length === 0) {
    container.appendChild(html('div', { className: 'empty' },
      html('h3', null, 'No speakers enrolled'),
      html('p', null, 'Go to the Meetings tab to enroll speakers from recorded meetings.')
    ));
    return;
  }

  const table = html('table', null,
    html('thead', null,
      html('tr', null,
        html('th', null, 'Name'),
        html('th', null, 'Enrollments'),
        html('th', null, 'Last Updated'),
        html('th', null, '')
      )
    )
  );

  const tbody = html('tbody');
  for (const spk of speakers) {
    const nameLink = html('a', {
      href: '#',
      className: 'speaker-name-link',
      onclick: (e) => { e.preventDefault(); showSpeakerDetail(spk.name); }
    }, spk.name);
    const row = html('tr', null,
      html('td', null, nameLink),
      html('td', null, String(spk.enrollments)),
      html('td', null, formatDate(spk.updated)),
      html('td', { style: 'text-align:right' },
        html('button', {
          className: 'btn btn-danger btn-sm',
          onclick: () => handleDeleteSpeaker(spk.name)
        }, 'Delete')
      )
    );
    tbody.appendChild(row);
  }
  table.appendChild(tbody);
  container.appendChild(table);
}

async function refreshSpeakers() {
  try {
    await fetchSpeakers();
    renderSpeakers();
  } catch (e) {
    toast('Failed to load speakers: ' + e.message);
  }
}

async function handleDeleteSpeaker(name) {
  const ok = await confirm('Delete Speaker', `Remove "${name}" and all their enrollments?`);
  if (!ok) return;
  try {
    await deleteSpeaker(name);
    toast(`Deleted "${name}"`);
    await refreshSpeakers();
  } catch (e) {
    toast('Delete failed: ' + e.message);
  }
}

async function handleResetAll() {
  const ok = await confirm('Reset All Speakers', 'This will remove all enrolled speaker profiles. This cannot be undone.');
  if (!ok) return;
  try {
    const result = await resetAllSpeakers();
    toast(`Removed ${result.removed} speaker(s)`);
    await refreshSpeakers();
  } catch (e) {
    toast('Reset failed: ' + e.message);
  }
}

async function handleBatchReidentify() {
  const ok = await confirm('Batch Re-identify',
    'Re-identify speakers in all past meetings using the current speaker database. Manually corrected labels will be preserved.');
  if (!ok) return;
  try {
    const result = await batchReidentify();
    toast(`Updated ${result.meetings_updated} of ${result.meetings_scanned} meeting(s)`);
    // Invalidate all cached meeting speakers
    for (const key of Object.keys(meetingSpeakerCache)) {
      delete meetingSpeakerCache[key];
    }
  } catch (e) {
    toast('Batch re-identify failed: ' + e.message);
  }
}

// ---------------------------------------------------------------------------
// Speaker Detail View
// ---------------------------------------------------------------------------

async function showSpeakerDetail(name) {
  const container = $('#content');
  container.innerHTML = '';

  const toolbar = html('div', { className: 'toolbar' },
    html('div', { style: 'display:flex;align-items:center;gap:12px' },
      html('button', { className: 'btn btn-sm', onclick: () => switchView('speakers') }, 'Back'),
      html('h2', null, name)
    ),
    html('button', {
      className: 'btn btn-danger btn-sm',
      onclick: async () => {
        const ok = await confirm('Delete Speaker', `Remove "${name}" and all their enrollments?`);
        if (!ok) return;
        try {
          await deleteSpeaker(name);
          toast(`Deleted "${name}"`);
          await fetchSpeakers();
          switchView('speakers');
        } catch (e) {
          toast('Delete failed: ' + e.message);
        }
      }
    }, 'Delete')
  );
  container.appendChild(toolbar);

  try {
    const detail = await api('GET', `/api/speakers/${encodeURIComponent(name)}`);

    const info = html('div', { className: 'card' },
      html('table', null,
        html('tbody', null,
          html('tr', null,
            html('td', { style: 'font-weight:600;width:140px' }, 'Enrollments'),
            html('td', null, String(detail.enrollments))
          ),
          html('tr', null,
            html('td', { style: 'font-weight:600' }, 'Embedding Dim'),
            html('td', null, String(detail.embedding_dim))
          ),
          html('tr', null,
            html('td', { style: 'font-weight:600' }, 'Created'),
            html('td', null, formatDate(detail.created))
          ),
          html('tr', null,
            html('td', { style: 'font-weight:600' }, 'Last Updated'),
            html('td', null, formatDate(detail.updated))
          )
        )
      )
    );
    container.appendChild(info);

    // Enrollment list with remove buttons
    if (detail.enrollments > 0) {
      const embCard = html('div', { className: 'card' },
        html('h3', { style: 'margin:0 0 12px' }, 'Enrollments')
      );
      for (let i = 0; i < detail.enrollments; i++) {
        const row = html('div', { className: 'speaker-row' },
          html('span', null, `Embedding #${i + 1}`),
          html('button', {
            className: 'btn btn-danger btn-sm',
            onclick: async (e) => {
              const ok = await confirm('Remove Enrollment',
                `Remove enrollment #${i + 1} from "${name}"? (${detail.enrollments - 1} remaining)`);
              if (!ok) return;
              try {
                const result = await removeEmbedding(name, i);
                toast(`Removed enrollment (${result.remaining} remaining)`);
                if (result.remaining === 0) {
                  await fetchSpeakers();
                  switchView('speakers');
                } else {
                  showSpeakerDetail(name);
                }
              } catch (err) {
                toast('Remove failed: ' + err.message);
              }
            }
          }, 'Remove')
        );
        embCard.appendChild(row);
      }
      container.appendChild(embCard);
    }
  } catch (e) {
    container.appendChild(html('div', { className: 'empty' },
      html('h3', null, 'Failed to load speaker details'),
      html('p', null, e.message)
    ));
  }
}

// ---------------------------------------------------------------------------
// Meetings View
// ---------------------------------------------------------------------------

function renderMeetings() {
  const container = $('#content');
  container.innerHTML = '';

  const toolbar = html('div', { className: 'toolbar' },
    html('h2', null, 'Meetings'),
    html('button', { className: 'btn', onclick: refreshMeetings }, 'Refresh')
  );
  container.appendChild(toolbar);

  if (meetings.length === 0) {
    container.appendChild(html('div', { className: 'empty' },
      html('h3', null, 'No meetings found'),
      html('p', null, 'Record a meeting with recmeet to see it here.')
    ));
    return;
  }

  for (const mtg of meetings) {
    const headerRight = html('span', { style: 'display:flex;align-items:center;gap:10px' },
      html('button', {
        className: 'btn btn-sm',
        onclick: () => openMeetingNote(mtg.name)
      }, 'View Note'),
      html('button', {
        className: 'btn btn-sm',
        onclick: () => showReprocessDialog(mtg.name)
      }, 'Reprocess'),
      html('span', { className: 'card-date' },
        mtg.has_speakers_json
          ? `${mtg.speaker_count} speaker(s)`
          : 'No speaker data'
      )
    );
    const card = html('div', { className: 'card', id: `meeting-${mtg.name}` },
      html('div', { className: 'card-header' },
        html('span', { className: 'card-title' }, mtg.name),
        headerRight
      )
    );

    if (mtg.has_speakers_json) {
      const speakersDiv = html('div', { className: 'meeting-speakers' });
      speakersDiv.textContent = 'Loading...';
      card.appendChild(speakersDiv);
      loadMeetingSpeakers(mtg.name, speakersDiv);
    } else {
      card.appendChild(html('div', { className: 'meeting-speakers', style: 'padding:8px 0;color:var(--fg-muted);font-size:0.85rem' },
        'No speakers.json — run with diarization enabled to generate speaker data.'
      ));
    }

    container.appendChild(card);
  }
}

async function loadMeetingSpeakers(dirName, container) {
  try {
    const spks = await fetchMeetingSpeakers(dirName);
    container.innerHTML = '';

    if (spks.length === 0) {
      container.textContent = 'No speakers detected.';
      return;
    }

    for (const spk of spks) {
      const row = html('div', { className: 'speaker-row' });

      // Label
      row.appendChild(html('span', { className: 'speaker-label' }, spk.label));

      // Duration
      row.appendChild(html('span', { className: 'speaker-duration' }, formatDuration(spk.duration_sec)));

      // Confidence badge
      if (spk.confidence > 0) {
        const confText = (spk.confidence * 100).toFixed(0) + '%';
        const confClass = spk.confidence >= 0.7 ? 'badge-success' : spk.confidence >= 0.5 ? 'badge-warning' : 'badge-danger';
        row.appendChild(html('span', { className: 'badge ' + confClass }, confText));
      }

      // Identified: show relabel button; Unidentified: show enroll form
      if (spk.identified) {
        row.appendChild(html('span', { className: 'badge badge-success' }, 'identified'));
        const relabelBtn = html('button', {
          className: 'btn btn-sm',
          onclick: () => showRelabelForm(row, relabelBtn, dirName, spk, container)
        }, 'Relabel');
        row.appendChild(relabelBtn);
      } else {
        const input = html('input', {
          className: 'enroll-input',
          type: 'text',
          placeholder: 'Name...'
        });
        const enrollBtn = html('button', {
          className: 'btn btn-primary btn-sm',
          onclick: async () => {
            const name = input.value.trim();
            if (!name) { toast('Enter a name'); return; }
            try {
              enrollBtn.disabled = true;
              enrollBtn.textContent = '...';
              const result = await enrollSpeaker(name, dirName, spk.cluster_id);
              let msg = `Enrolled "${name}"`;
              if (result && result.warning) {
                msg += ' — ' + result.warning;
              }
              toast(msg);
              await fetchSpeakers();
              await loadMeetingSpeakers(dirName, container);
            } catch (e) {
              toast('Enroll failed: ' + e.message);
              enrollBtn.disabled = false;
              enrollBtn.textContent = 'Enroll';
            }
          }
        }, 'Enroll');

        input.addEventListener('keydown', (e) => {
          if (e.key === 'Enter') enrollBtn.click();
        });

        row.appendChild(input);
        row.appendChild(enrollBtn);
      }

      container.appendChild(row);
    }
  } catch (e) {
    container.textContent = 'Failed to load speakers.';
  }
}

function showReprocessDialog(dirName) {
  const input = html('input', {
    type: 'number', min: '0', max: '20', value: '',
    placeholder: 'auto', style: 'width:80px'
  });
  const overlay = html('div', { className: 'overlay' },
    html('div', { className: 'dialog' },
      html('h3', null, 'Reprocess Meeting'),
      html('p', null, `Re-diarize "${dirName}" with a speaker count hint.`),
      html('div', { style: 'margin:12px 0' },
        html('label', { style: 'font-weight:600' }, 'Number of speakers: '),
        input
      ),
      html('div', { className: 'dialog-actions' },
        html('button', { className: 'btn', onclick: () => overlay.remove() }, 'Cancel'),
        html('button', {
          className: 'btn btn-primary',
          onclick: async () => {
            const num = parseInt(input.value) || 0;
            overlay.remove();
            try {
              const result = await reprocessMeeting(dirName, num);
              toast(`Reprocessing started (job #${result.job_id})`);
            } catch (e) {
              toast('Reprocess failed: ' + e.message);
            }
          }
        }, 'Reprocess')
      )
    )
  );
  document.body.appendChild(overlay);
  input.focus();
}

function showRelabelForm(row, btn, dirName, spk, container) {
  btn.style.display = 'none';
  const input = html('input', {
    className: 'enroll-input',
    type: 'text',
    value: spk.label
  });
  const updateChk = html('input', { type: 'checkbox', checked: 'checked' });
  const chkLabel = html('label', { style: 'font-size:0.8rem;display:flex;align-items:center;gap:4px' },
    updateChk, 'Update profiles'
  );
  const saveBtn = html('button', {
    className: 'btn btn-primary btn-sm',
    onclick: async () => {
      const name = input.value.trim();
      if (!name) { toast('Enter a name'); return; }
      try {
        saveBtn.disabled = true;
        saveBtn.textContent = '...';
        await relabelSpeaker(dirName, spk.cluster_id, name, updateChk.checked);
        toast(`Relabeled to "${name}"`);
        await fetchSpeakers();
        await loadMeetingSpeakers(dirName, container);
      } catch (e) {
        toast('Relabel failed: ' + e.message);
        saveBtn.disabled = false;
        saveBtn.textContent = 'Save';
      }
    }
  }, 'Save');
  const cancelBtn = html('button', {
    className: 'btn btn-sm',
    onclick: () => {
      input.remove(); chkLabel.remove(); saveBtn.remove(); cancelBtn.remove();
      btn.style.display = '';
    }
  }, 'Cancel');
  input.addEventListener('keydown', (e) => { if (e.key === 'Enter') saveBtn.click(); });

  row.appendChild(input);
  row.appendChild(chkLabel);
  row.appendChild(saveBtn);
  row.appendChild(cancelBtn);
  input.focus();
  input.select();
}

async function openMeetingNote(dirName) {
  try {
    const data = await api('GET', `/api/meetings/${encodeURIComponent(dirName)}/note`);
    const tab = window.open('', '_blank');
    if (!tab) { toast('Pop-up blocked — allow pop-ups for this site'); return; }

    // Simple markdown-to-HTML: headings, bold, blockquotes, paragraphs
    const md = data.content;
    let inFrontmatter = false;
    let frontmatterDone = false;
    const lines = md.split('\n');
    const parts = [];

    for (let i = 0; i < lines.length; i++) {
      const line = lines[i];

      // Strip YAML frontmatter
      if (line.trim() === '---') {
        if (!frontmatterDone) { inFrontmatter = !inFrontmatter; if (!inFrontmatter) frontmatterDone = true; continue; }
      }
      if (inFrontmatter) continue;

      // Headings
      const hMatch = line.match(/^(#{1,4})\s+(.*)/);
      if (hMatch) {
        const level = hMatch[1].length;
        parts.push(`<h${level}>${esc(hMatch[2])}</h${level}>`);
        continue;
      }

      // Blockquote lines (Obsidian callouts and quoted text)
      if (line.startsWith('> ')) {
        const inner = line.slice(2);
        // Callout header: > [!type] Title or > [!type]- Title
        const callout = inner.match(/^\[!(\w+)\]-?\s*(.*)/);
        if (callout) {
          parts.push(`<div class="callout callout-${callout[1]}"><strong>${esc(callout[2] || callout[1])}</strong>`);
          continue;
        }
        parts.push(`<div class="bq">${renderInline(inner)}</div>`);
        continue;
      }

      // Blank line — close any open callout
      if (line.trim() === '') {
        parts.push('<br>');
        continue;
      }

      // Checkbox lines
      if (line.startsWith('- [ ] ')) {
        parts.push(`<div class="todo">${renderInline(line.slice(6))}</div>`);
        continue;
      }

      // Regular text
      parts.push(`<p>${renderInline(line)}</p>`);
    }

    tab.document.write(`<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>${esc(data.path)}</title>
<style>
  body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
         max-width: 800px; margin: 40px auto; padding: 0 24px; line-height: 1.6;
         color: #1a1a2e; background: #fff; }
  @media (prefers-color-scheme: dark) {
    body { color: #e2e8f0; background: #1a1a2e; }
    .callout { background: #16213e; border-color: #334155; }
    .bq { color: #94a3b8; }
    .todo { color: #94a3b8; }
  }
  h1 { font-size: 1.5rem; margin: 24px 0 8px; }
  h2 { font-size: 1.25rem; margin: 20px 0 8px; }
  h3 { font-size: 1.1rem; margin: 16px 0 6px; }
  p, .bq, .todo { margin: 2px 0; font-size: 0.95rem; }
  .bq { padding-left: 12px; border-left: 3px solid #6366f1; color: #6c757d; }
  .callout { padding: 12px 16px; margin: 8px 0; border-radius: 6px;
             background: #f8f9fa; border-left: 4px solid #6366f1; }
  .todo::before { content: "\\2610 "; }
  strong { font-weight: 600; }
  br { display: block; content: ""; margin: 4px 0; }
</style></head><body>${parts.join('\n')}</body></html>`);
    tab.document.close();
  } catch (e) {
    toast('Note not found — meeting may not have been summarized yet');
  }
}

function esc(s) {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

function renderInline(s) {
  // Bold (**text**), then escape HTML in non-bold parts
  return s.replace(/\*\*(.+?)\*\*/g, '<strong>$1</strong>')
          .replace(/`([^`]+)`/g, '<code>$1</code>');
}

async function refreshMeetings() {
  try {
    // Clear cache
    for (const key of Object.keys(meetingSpeakerCache)) {
      delete meetingSpeakerCache[key];
    }
    await fetchMeetings();
    renderMeetings();
  } catch (e) {
    toast('Failed to load meetings: ' + e.message);
  }
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

function switchView(view) {
  currentView = view;
  for (const btn of $$('.nav button')) {
    btn.classList.toggle('active', btn.dataset.view === view);
  }
  if (view === 'speakers') {
    renderSpeakers();
  } else {
    renderMeetings();
  }
}

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

function formatDate(iso) {
  if (!iso) return '';
  const d = new Date(iso);
  if (isNaN(d.getTime())) return iso;
  return d.toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' });
}

function formatDuration(sec) {
  if (sec == null || sec <= 0) return '';
  const m = Math.floor(sec / 60);
  const s = Math.round(sec % 60);
  return m > 0 ? `${m}m ${s}s` : `${s}s`;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

async function init() {
  // Nav handlers
  for (const btn of $$('.nav button')) {
    btn.addEventListener('click', () => switchView(btn.dataset.view));
  }

  // Load initial data
  try {
    await Promise.all([fetchSpeakers(), fetchMeetings()]);
  } catch (e) {
    toast('Failed to connect to server');
  }

  renderSpeakers();
}

document.addEventListener('DOMContentLoaded', init);
