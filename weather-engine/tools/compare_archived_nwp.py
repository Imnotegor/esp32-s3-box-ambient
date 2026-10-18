"""Compare already-trained holdout predictions with an issued NWP archive.

The archive contains examples with issued_at (availability time), valid_at,
baseline [temperature C, pressure hPa] and truth in the same units. It is read
only by this evaluation tool, never by model training or inference.
"""

import argparse
import hashlib
import json
from pathlib import Path


def compare(state: Path, archive: Path, delay_hours: int) -> dict:
    if not 0 <= delay_hours <= 24:
        raise ValueError("publication delay must be between 0 and 24 hours")
    generation = json.loads((state / "active-model.json").read_text())
    if not isinstance(generation, str) or not generation.replace("-", "").isdigit():
        raise ValueError("invalid model generation")
    directory = state / "models" / generation
    manifest = json.loads((directory / "manifest.json").read_text())
    raw_dataset = (state / "dataset.json").read_bytes()
    if hashlib.sha256(raw_dataset).hexdigest() != manifest["dataset_sha256"]:
        raise ValueError("dataset differs from the model's training/evaluation data")
    data = json.loads(raw_dataset)
    forecasts = json.loads((directory / "test-predictions.json").read_text())
    cutoff = manifest["report"]["split_cutoffs"][2]
    if any(row["origin"] < cutoff for row in forecasts):
        raise ValueError("saved predictions contain non-holdout origins")
    predictions = {row["origin"]: row["values"] for row in forecasts}
    # The saved schema has nine locations, seven variables each, then four
    # calendar features. The central location starts at channel 4 * 7.
    truth = {
        timestamp: data["values"][index * 67 + 28 : index * 67 + 30]
        for index, timestamp in enumerate(data["times"])
    }
    totals = [[0.0, 0.0], [0.0, 0.0]]
    count = 0
    for row in json.loads(archive.read_text())["examples"]:
        origin = row["issued_at"] - delay_hours * 3600
        lead_seconds = row["valid_at"] - origin
        if origin not in predictions or lead_seconds % 3600:
            continue
        lead = lead_seconds // 3600
        if not 1 <= lead <= len(predictions[origin]):
            continue
        label = truth[row["valid_at"]]
        if any(abs(a - b) > 0.001 for a, b in zip(label, row["truth"])):
            raise ValueError("archive and independent model use different labels")
        for model, prediction in enumerate(
            (predictions[origin][lead - 1], row["baseline"])
        ):
            for variable in range(2):
                totals[model][variable] += abs(prediction[variable] - label[variable])
        count += 1
    if not count:
        raise ValueError("no matching held-out initialization/valid times")
    return {
        "comparison": "same initialization/valid times; NWP is evaluation-only",
        "publication_delay_hours": delay_hours,
        "matched_cases": count,
        "label_mismatches": 0,
        "independent_mae": [value / count for value in totals[0]],
        "nwp_mae": [value / count for value in totals[1]],
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-dir", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--publication-delay-hours", type=int, required=True)
    args = parser.parse_args()
    print(
        json.dumps(
            compare(args.state_dir, args.archive, args.publication_delay_hours),
            indent=2,
        )
    )
