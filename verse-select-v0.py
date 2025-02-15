import json
import csv
import os
import datetime
from colorama import init, Fore, Style

# Initialize colorama for colored terminal output.
init(autoreset=True)

# Define the canonical order for the 66 books in the NIV.
canonical_books = [
    "Genesis", "Exodus", "Leviticus", "Numbers", "Deuteronomy",
    "Joshua", "Judges", "Ruth", "1 Samuel", "2 Samuel",
    "1 Kings", "2 Kings", "1 Chronicles", "2 Chronicles", "Ezra",
    "Nehemiah", "Esther", "Job", "Psalm", "Proverbs",
    "Ecclesiastes", "Song of Solomon", "Isaiah", "Jeremiah", "Lamentations",
    "Ezekiel", "Daniel", "Hosea", "Joel", "Amos",
    "Obadiah", "Jonah", "Micah", "Nahum", "Habakkuk",
    "Zephaniah", "Haggai", "Zechariah", "Malachi",
    "Matthew", "Mark", "Luke", "John", "Acts",
    "Romans", "1 Corinthians", "2 Corinthians", "Galatians", "Ephesians",
    "Philippians", "Colossians", "1 Thessalonians", "2 Thessalonians", "1 Timothy",
    "2 Timothy", "Titus", "Philemon", "Hebrews", "James",
    "1 Peter", "2 Peter", "1 John", "2 John", "3 John",
    "Jude", "Revelation"
]

# Define the local file name for your NIV Bible JSON.
bible_json_file = "/Users/boazburnett/PycharmProjects/Bible-clock-v0/NIV_bible.json"

# Verify the file exists and is not empty.
if not os.path.exists(bible_json_file) or os.path.getsize(bible_json_file) == 0:
    raise Exception("NIV Bible JSON file not found or is empty.")

# Load the NIV Bible JSON.
with open(bible_json_file, "r", encoding="utf-8") as file:
    bible_text = json.load(file)

def find_valid_verses(chapter, verse):
    """
    Find books that contain the given chapter:verse reference.
    The results are sorted according to the canonical order defined above.
    """
    valid_verses = []
    for book, chapters in bible_text.items():
        if str(chapter) in chapters and str(verse) in chapters[str(chapter)]:
            valid_verses.append((book, chapter, verse, bible_text[book][str(chapter)][str(verse)]))
    # Sort the verses so that the books appear in canonical NIV order.
    valid_verses.sort(key=lambda x: canonical_books.index(x[0]) if x[0] in canonical_books else float('inf'))
    return valid_verses if valid_verses else []

def select_favorite_verse(time_slot, options):
    """
    Displays all verse options with alternating colors and lets the user pick a favorite.
    """
    print(f"\nSelect a verse for {time_slot} (Chapter:Verse = {time_slot}):\n")
    for i, (book, ch, v, verse_text) in enumerate(options, 1):
        color = Fore.BLUE if i % 2 == 1 else Fore.WHITE
        print(f"{color}{i}. {book} {ch}:{v} - {verse_text}{Style.RESET_ALL}")
    while True:
        choice = input("Enter the number of your favorite verse (or type 'skip' to skip, 'end' to save and exit): ").strip().lower()
        if choice == "skip":
            return None
        if choice == "end":
            return "end"
        if choice.isdigit() and 1 <= int(choice) <= len(options):
            return options[int(choice) - 1]
        print("Invalid choice. Please enter a valid number.")

def generate_hourly_verses(start_hour=0, start_minute=0):
    """
    Iterate through all times and let the user select their favorite verses,
    starting at a specified time.
    """
    selected_verses = []
    print("You can start at a specific time by typing in the format HH:MM (e.g., 02:01). Press Enter to start at 00:00.")
    start_time = input("Enter start time (HH:MM or press Enter to start at 00:00): ").strip()
    if start_time:
        try:
            start_hour, start_minute = map(int, start_time.split(":"))
        except ValueError:
            print("Invalid time format. Defaulting to 00:00.")
            start_hour, start_minute = 0, 0

    for hour in range(start_hour, 24):
        for minute in range(start_minute if hour == start_hour else 0, 60):
            time_slot = f"{hour:02d}:{minute:02d}"
            chapter = hour
            verse = minute
            verse_options = find_valid_verses(chapter, verse)
            
            if not verse_options:
                print(f"No valid verse found for {time_slot}. Skipping...")
                continue

            selected = select_favorite_verse(time_slot, verse_options)
            if selected == "end":
                print("Ending early and saving progress...")
                return selected_verses
            if selected:
                book, ch, v, verse_text = selected
                selected_verses.append([time_slot, book, ch, v, verse_text])

    return selected_verses

# Run the verse selection process.
selected_verses = generate_hourly_verses()

# Generate a timestamped filename.
timestamp = datetime.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
csv_filename = f"selected_verses_{timestamp}.csv"

# Save the selected verses to a CSV file.
with open(csv_filename, "w", newline="", encoding="utf-8") as file:
    writer = csv.writer(file)
    writer.writerow(["Time", "Book", "Chapter", "Verse", "Text"])
    writer.writerows(selected_verses)

print(f"\nSelection process complete! Your chosen verses are saved in {csv_filename}.")
