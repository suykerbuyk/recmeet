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
  await api('POST', '/api/speakers/enroll', {
    name, meeting_dir: meetingDir, cluster_id: clusterId
  });
  // Invalidate cache for this meeting
  delete meetingSpeakerCache[meetingDir];
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
    const row = html('tr', null,
      html('td', null, spk.name),
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
    const card = html('div', { className: 'card', id: `meeting-${mtg.name}` },
      html('div', { className: 'card-header' },
        html('span', { className: 'card-title' }, mtg.name),
        html('span', { className: 'card-date' },
          mtg.has_speakers_json
            ? `${mtg.speaker_count} speaker(s)`
            : 'No speaker data'
        )
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

      // Identified badge or enroll form
      if (spk.identified) {
        row.appendChild(html('span', { className: 'badge badge-success' }, 'identified'));
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
              await enrollSpeaker(name, dirName, spk.cluster_id);
              toast(`Enrolled "${name}"`);
              // Refresh both views
              await fetchSpeakers();
              await loadMeetingSpeakers(dirName, container);
            } catch (e) {
              toast('Enroll failed: ' + e.message);
              enrollBtn.disabled = false;
              enrollBtn.textContent = 'Enroll';
            }
          }
        }, 'Enroll');

        // Allow Enter key to submit
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
