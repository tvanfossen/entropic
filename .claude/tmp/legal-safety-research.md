# Legal/Safety Protections Research for Entropic

## Comparison Table: Open-Source AI Inference Tools

| Feature | llama.cpp | ollama | vLLM | LocalAI | text-gen-webui |
|---|---|---|---|---|---|
| **License** | MIT | MIT | Apache-2.0 | MIT | AGPL-3.0 |
| **NOTICE/DISCLAIMER file** | No | No | No | No | No |
| **AI output disclaimer** | No | No | No | No | No |
| **Privacy/telemetry docs** | No | No | No | No | No |
| **Responsible use policy** | No | No | No | No | No |
| **Content filtering docs** | No | No | No | No | No |
| **SECURITY.md** | Yes | Yes | Yes | Yes | No |
| **CODE_OF_CONDUCT.md** | No | No | Yes | No | No |

## Key Finding

**None of the 5 major open-source AI inference tools include any legal protections beyond the standard license warranty disclaimer.** No AI-specific disclaimers, no responsible use policies, no output disclaimers.

All rely solely on the standard "AS IS" warranty disclaimer in their license files (MIT or Apache-2.0).

## What the License Already Covers (Apache-2.0 for Entropic)

Apache-2.0 already includes:
- **Section 7**: No warranty — "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND
- **Section 8**: Limitation of liability — not liable for damages
- Patent grant with defensive termination

## Recommendations for Entropic (Prioritized)

### 1. SECURITY.md (HIGH — industry standard)
All 4 of 5 tools have this. Entropic should too. Template:
- How to report vulnerabilities
- Supported versions
- Response timeline expectations

### 2. AI Output Disclaimer in README (MEDIUM — differentiator)
No peer tools do this, but it's smart practice:
> "Entropic runs AI models locally on your hardware. AI-generated outputs may be inaccurate, biased, or inappropriate. Users are solely responsible for evaluating and using any generated content. This software does not provide professional, legal, medical, or financial advice."

### 3. Privacy Statement in README (MEDIUM — marketing advantage)
Entropic's local-first nature is a selling point. Document it:
> "Entropic runs entirely on your local hardware. No data is sent to external servers. No telemetry is collected. Your prompts, conversations, and model outputs never leave your machine."

### 4. NOTICE file (LOW — Apache-2.0 convention)
Apache-2.0 supports a NOTICE file for attribution. Low priority but good practice if using third-party code.

### 5. Responsible Use Statement (LOW — no peers do this)
Could add to docs, but no precedent among peer tools. Consider a lightweight statement rather than a full policy.

### NOT Recommended
- **Separate LICENSE files for AI output** — no legal basis, no precedent
- **Content filtering built-in** — Entropic is an inference engine, not a model provider. Model-level safety is the model's responsibility.
- **DMCA policy** — only relevant for hosted services, not local tools
- **Terms of Service** — only for SaaS products, not open-source tools
